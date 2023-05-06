/*
 * Create a file similar to BEMC.SmartHub hourly detail usage from emonesp data.
 *
 * The emonesp connects to my Mosquitto broker sending it's data.
 *
 * Licenced under GNU GPL V3.0 see https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Author  MK Beavers
 * Date    2023.May.1
 * Version 1.0.0
 * Change History:
 *    2023 May 2 - Initial release
 */
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <unistd.h>
#include <mosquitto.h>

static double            prev_watts = 0;
static double           accum_watts = 0;
static FILE                   *  db = NULL;
static struct timespec  next_lower_bound = {0};
static struct timespec  prev_sample = {0};
static int       DEBUG = 0, VERBOSE = 0;            //control stdout data
static int               pipe_wr_fd = 0;                    //write-end of the pipe
static int               pipe_rd_fd = 0;                //read-end of the pipe


const  char * time_format = "%Y-%m-%d_%H:%M";       //as used by strftime()
const  int   month_offset = 5;                      //in time_format
const  int    fifteen_min = 15 * 60;                //in seconds
const  double     hour_ms = 1.0 / (60 * 60 * 1000); // 1 / min * sec * ms
const  double       w2kwh = 1000;                   // wattHours to kWh
const  long         ns2ms = 1000000;              // nanoseconds to milliseconds
const  int      write_end = 1;                      //pipe_wr_offset
const  int       read_end = 0;                      //pipe_rd_offset


// process connecting reply and subscribe to emonesp messages
void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
        printf("ID: %d\n", * (int *) obj);
        if(rc)
        {
                printf("Error with result code: %d\n", rc);
                exit(-1);
        }
        mosquitto_subscribe(mosq, NULL, "emon/emonesp/#", 0);
}


// return time in milliseconds  (allow for overflow)
long diff(struct timespec * start, struct timespec * end)
{
    static  int first_time = 1;
    if (first_time)             // hack for the very first sample
    {
        first_time = 0;
        return 1000;            // assumed 1 second sample rate
    }

    long  temp;
    if ((end->tv_nsec - start->tv_nsec)<0)
    {
        temp = 1000000000 * (end->tv_sec - start->tv_sec - 1);    //time_t
        temp += 1000000000 + end->tv_nsec - start->tv_nsec;
    }
    else
    {
        temp = 1000000000 * (end->tv_sec - start->tv_sec);
        temp += end->tv_nsec - start->tv_nsec;
    }
    return temp / ns2ms;
}


//https://forums.codeguru.com/showthread.php?343829-break-blocked-getchar-call
// Wait until either stdin or pipe_rd_fd has data available.
// Returns: true stdin has data
//          false pipe_rd_rd has data
bool inputReadyWait()
{
        if (!pipe_wr_fd)
        {
                // no block/unblock pipe yet....
                int unblock_pipe[2];
                if (pipe(unblock_pipe))
                {
            fprintf(stderr, "pipe() failed, errno=%d.\n", errno);
                        exit(-1);
                }
                pipe_wr_fd = unblock_pipe[write_end];           // unblock key
                pipe_rd_fd = unblock_pipe[read_end];        // block check
        }

        // find out if (input_is_ready_on_stdin || pipe_rd_fd_has_been_activated)
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        FD_SET(pipe_rd_fd, &read_set);  // if there's anything to read on the
                                    // pipe_rd_rd has been activated!

        // wait until either input is ready
        if (-1 == select(1 + pipe_rd_fd, &read_set, NULL, NULL, NULL))
        {
        fprintf(stderr, "select() failed, errno=%d.\n", errno);
                exit(-1);
        }

        if (FD_ISSET(STDIN_FILENO, &read_set))
                return true;                        // input ready on stdin
        else
        {
                if (pipe_rd_fd)                 // user issued cancel wait!
                        close(pipe_rd_fd);
                pipe_rd_fd = 0;
                return false;
        }
}


// send data to the pipe making pipe_rd_fd ready, select() will unblock.
void cancelInputWait()
{
        if (pipe_wr_fd)
        {
                write(pipe_wr_fd, '\0', 1);     // stub!
                close(pipe_wr_fd);
                pipe_wr_fd = 0;
        }
}


//  Accumulate the watt samples and print kWh used every 15 minutes.
//  Halt this program when the period upper_bound is in a new month
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
    if (strcmp("emon/emonesp/W", msg->topic))
    {
        if (VERBOSE)
            printf("%s \t%s \n", msg->topic, (char *)msg->payload);
    }
    else
    {
        double              current_watts = atof((char*)msg->payload);
        double              average_watts = (current_watts + prev_watts) / 2.0;
        time_t              start_interval;
        struct timespec     now;
        struct tm         * clock;
        char                report[100];

        clock_gettime(CLOCK_REALTIME, &now);
        long    sample_ms = diff(&prev_sample, &now);
        accum_watts += (average_watts * hour_ms * sample_ms);
        if (DEBUG)
            printf("current_watts(%.2f) sample_ms(%ld) accum_watts(%.2f) t=%s",
                    current_watts, sample_ms, accum_watts, ctime(&now.tv_sec));
        prev_sample = now;
        prev_watts = current_watts;
        // if this measurment period is complete save the results
        if (now.tv_sec >= next_lower_bound.tv_sec)
        {
            char   period_start[20];
            char    old_month[2];
            start_interval = next_lower_bound.tv_sec - fifteen_min;
            clock = localtime(&start_interval);
            strftime(period_start, sizeof(period_start), time_format, clock);
            sprintf(report, "%s,%f\n", period_start, accum_watts / w2kwh);
            fprintf(db, report);
            fflush(db);
            printf(report);
            next_lower_bound.tv_sec += fifteen_min;
            accum_watts = 0;
            //check for new month & if true cancel input wait
            memcpy(old_month, &period_start[month_offset], 2);  //len(MM)
            clock = localtime(&next_lower_bound.tv_sec);
            strftime(period_start, sizeof(period_start), time_format, clock);
            if (memcmp(old_month, &period_start[month_offset], 2))
            {
                cancelInputWait();
            }
        }
    }
}


// establish connection to broker and open the output destination
int main(int argc, char *argv[])
{
        int     rc, opt, USAGE = 0, id = 2362;
        char  * USER = NULL, * PSWD = NULL, * BROKER = NULL, * OUTFILE = "./watts";
        extern char *optarg;
        extern int optind;

        while ((opt = getopt(argc, argv, "dvu:P:b:")) != -1) {

        switch (opt) {
        case 'b': BROKER = optarg; break;
        case 'd': DEBUG = 1; break;
        case 'h': USAGE = 1; break;
        case 'P': PSWD = optarg; break;
        case 'u': USER = optarg; break;
        case 'v': VERBOSE = 1; break;
        default:  USAGE = 1;            // if unknown show usage message
        }
    }
    if (optind + 1 == argc)     /* arguments after the command-line options */
                OUTFILE = argv[optind];

        if (USAGE || USER==NULL || PSWD==NULL || BROKER==NULL)
        {
        fprintf(stderr, "Usage: %s -b:u:P:[dhv] [file] \n", argv[0]);
        fprintf(stderr, "  required flags b=broker, u=user, P=password \n");
        fprintf(stderr, "  optional flags [d=debug, h=help, v=verbose] \n");
        fprintf(stderr, "  optional file=output (default: ./watts(datetime).txt) \n");
        return 1;
        }

    setlocale(LC_NUMERIC, "");
        mosquitto_lib_init();
        struct mosquitto *mosq = mosquitto_new("Compute_Usage", true, &id);
        mosquitto_connect_callback_set(mosq, on_connect);
        mosquitto_message_callback_set(mosq, on_message);
        mosquitto_username_pw_set(mosq, USER, PSWD);
        rc = mosquitto_connect(mosq, BROKER, 1883, 10);
        if(rc) {
                fprintf(stderr, "Could not connect to Broker, return code %d\n", rc);
                return -1;
        }

    /* Compute the period start (lower bound) */
        struct tm         * clock;
        char     period_start[20];
        char        filename[100];

    int     adjustment = 0;
    clock_gettime(CLOCK_REALTIME, &next_lower_bound);
    clock = localtime(&next_lower_bound.tv_sec);
    if (clock->tm_sec != 0)
    {
        adjustment += 60 - clock->tm_sec;
        clock->tm_min += 1;
    }
    if (clock->tm_min % 15 != 0) adjustment += 60 * (15 - clock->tm_min % 15);
    next_lower_bound.tv_sec += adjustment - fifteen_min;
        printf("Period begins at %s\n", ctime(&next_lower_bound.tv_sec));

        //construct for filename with
        strftime(period_start, sizeof(period_start), time_format, clock);
        sprintf(filename, "%s%s%s", OUTFILE, period_start, ".txt");

        // Open the file for writing
        db = fopen(filename, "w");
        if(NULL == db)
        {
                fprintf(stderr, "Could not open the database, errno=%d.\n", errno);
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
                return -1;
        }
        //make adjustment for detecting the end of sample period
        next_lower_bound.tv_sec += fifteen_min;

        //write header to output destination
        fprintf(db, "# Start of energy consumption 15 minute period, kWh used \n");
        //start the message loop
        mosquitto_loop_start(mosq);
        printf("Press Enter to quit...\n");
        //blocking wait (on_message will normally free the block)
        inputReadyWait();
        //clean up resources before exit
        mosquitto_loop_stop(mosq, true);
    fprintf(db, "#   Sentinel  halt  stop\n");
        fflush(db);
        fclose(db);
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();

        return 0;
}


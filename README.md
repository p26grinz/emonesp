# emonesp
Save the watt usage data.

My electric utility (BEMC) has a SmartHub web app that allows me to download my electric usage data.  
The electric usage data is provided in 15 minute periods aligned at 00, 15, 30, 45 minutes.  
I've chosen to download data on a monthly bases.

This program mimics the SmartHub format for the most part.
A record begins with the timestamp of the lower bound for the period then reports the kWh for the period.
The program ends when either the user press Enter from the console or the final period for a month is recorded.

I my environment I've set a cron job to start the program at Midnight on day 1 of each month.
I've got a Raspberry Pi 4B with SSD recording my data.  Seems to work satisfactorily.

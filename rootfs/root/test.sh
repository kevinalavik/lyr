curl -o /dev/null -w 'download=%{size_download} bytes\nspeed=%{speed_download} bytes/s\ntime=%{time_total}s\n' \
  http://speedtest.tele2.net/100MB.zip

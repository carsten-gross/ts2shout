# ts2shout
ts2shout is a small software that acts as a filter or cgi programm to convert a single
program mpeg transport stream directly to a webradio compatible shoutcast stream. 
I actually use it for playing DVB-C and DVB-S on Squeezebox players. Please see my page 
http://www.siski.de/~carsten/radio-streaming-squeezebox.html for more information.

It is based on the dvbshout application by Nicholas J. Humfrey
(https://github.com/njh/dvbshout) - but the use case here is completly
different. ts2shout uses autoconfiguration by reading PAT and PMT tables from a
single program mpeg transport stream and outputs a native mpeg (or shoutcast) stream 
directly.

There are two modes, one is very like a traditional unix filter: a mpeg transport stream
is read from stdin and it is filtered to stdout. Error and info messages are directed to 
sterr. 

The other mode is CGI (common gateway interface) mode. This avoids hasseling
around with shoutcast like parameters (like icy-sr, icy-br etc.) which are read
directly from the mpeg transport stream and output in the corresponding and
required header lines. To support this mode libcurl is used for accessing
tvheadend. This means libcurl (and the corresponding libcurl-dev package for
compiling) is required for ts2shout to work. In CGI mode it is expected that
you have a tvheadend or a similar streaming server available that provides the
MPEG transport stream by a network protocol libcurl understands. Please see for 
a configuration example for apache2 below.

ts2shout outputs a shoutcast stream with the first found mpeg audio stream in
PAT/PMT and the "current programm" from the mpeg EIT ("EPG") translated to
"StreamTitle" all 8192 bytes inside the mpeg stream. This is a standard for
most shoutcast radio-stations and described in
http://www.smackfu.com/stuff/programming/shoutcast.html. ts2shout should work
with most DVB-C/DVB-S radio stations and was tested by myself with Unitymedia
DVB-C and Astra 19.2E DVB-S satellite reception.

Just compile the application with make on your linux box and install it
manually e.g. to /usr/local/bin/ts2shout . It does not need any libraries for
shoutcast or mpeg as it just parses and uses a sub-extreme-minimum set of mpeg
specification just to work for me in my special setup to get stream information
on the Squeezebox.

Some german cultural programmes also have AC3 streams available. Examples are
"Bayern 4 Klassik" or "SWR2". If you supply the command line parameter "ac3" or
(in CGI mode) the value "1" in the environment variable "AC3" you'll get an AC3
stream. This currently doesn't work with the squeezebox but you can fetch the stream 
and it can be decoded by ffmpeg or played with mplayer.

ts2shout can be used in conjunction with tvheadend and apache2 as follows: 

Activate module mod_action (a2enmod action) on your apache2

On a Debian system change /etc/apache2/sites-available/000-default.conf as follows. Use a virtual server
from your apache2 installation as you wish. 

It assumes that your tvheadend is reachable via localhost. Change and adjust the radio
URLs, the tvheadend URLs and the allow/deny lists as you need them. 

If shoutcast Streamtitles should be used inside the stream is autodetected by
the Icy-MetaData header given in the http request. 

<pre>
	&lt;Location /radio&gt;
		Order Deny,Allow
		Deny from all
		Allow from 172.16.0.0/24
		Allow from ::1
		Allow from 127.0.0.1
		&lt;If "%{HTTP:Icy-MetaData} in {'1'}"&gt;
			SetEnv "MetaData" "1"
		&lt;/If&gt;
		SetEnv TVHEADEND "http://localhost:9981/stream/channelnumber"
		# All radio stations. One line for every station
		# the first name is the uri, PROGRAMMNO is the environment
		# setting needed by ts2shout. 
		# A radio is accessed with the URI local part for example
		# /radio/swr1bw
		# this fetches the mpeg transport from http://localhost:9981/stream/channelnumber/813
		SetEnvIf REQUEST_URI "swr1bw$" PROGRAMMNO=813
		SetEnvIf REQUEST_URI "swr2$" PROGRAMMNO=815
		# As many as you need
		SetEnvIf REQUEST_URI "drs2$" PROGRAMMNO=913
		SetEnvIf REQUEST_URI "drs3$" PROGRAMMNO=914
		Options +ExecCGI
		Action ts2shout /cgi-bin/ts2shout virtual
		SetHandler ts2shout
	&lt;/Location&gt;
</pre>


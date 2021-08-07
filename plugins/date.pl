
#
#	Example 'date' plugin
#	Heikki Hannikainen <hessu@hes.iki.fi>
#	Wed Sep  6 15:45:19 EEST 2000
#

# command function for 'date'
sub plugin_date {
	my($num, @argv) = @_;
	spoolsms($num, scalar(localtime(time())) . " local");
}

# add the 'date' and 'time' commands to the commands hash
$commands{date} = 'plugin_date';
$commands{time} = 'plugin_date';

# Do not add any initialisation code here, do everything in the plugin
# command, since everything else is executed every time the handler is run,
# nevermind what command the user has specified.

1;


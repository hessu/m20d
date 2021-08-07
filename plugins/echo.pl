
#
#	Example 'echo' plugin
#	Heikki Hannikainen <hessu@hes.iki.fi>
#	Wed Sep  6 15:45:19 EEST 2000
#

# command function for 'echo'
sub plugin_echo {
	my($num, @argv) = @_;
	# send the arguments back
	spoolsms($num, join(' ', @argv));
}

# add the 'echo' command to the commands hash
$commands{echo} = 'plugin_echo';

# Do not add any initialisation code here, do everything in the plugin
# command, since everything else is executed every time the handler is run,
# nevermind what command the user has specified.

1;


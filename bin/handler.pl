#!/usr/bin/perl

$spool = "/opt/m20d/spool";
$plugins = "/opt/m20d/plugins";

use Fcntl;

my($msgid, $src, $tmpf) = @ARGV;

open(F, $tmpf) || die "[$msgid] Could not open spool file $tmpf: $!";
$len = read(F, $msg, 2000);
if (!defined($len)) { die "Could not read from spool file $tmpf: $!"; }
close(F) || die "[$msgid] Could not close spool file $tmpf after reading: $!";
#if (unlink($tmpf) ne 1) { die "Could not unlink spool file $tmpf: $!"; }

opendir(D, $plugins) || die "Could not opendir $plugins: $!";
@plugfiles = grep { /^([^.].*\.pl)$/ } readdir(D);
closedir(D) || die "Could not closedir $plugins: $!";

foreach $plug (@plugfiles) {
	do "$plugins/$plug" || die "Could not do $plug: $!";
}

($cmd, @args) = split(/\s+/, $msg);
$cmd = lc($cmd);

if (!defined($commands{$cmd})) {
	# Don't respond by default! spoolsms($src, "Keep your tunkki.");
} else {
	&{ $commands{$cmd} } ($src, @args);
}

########################################################################

#
# Send an SMS by creating a spool file, be careful not to overwrite an old one
# (call me paranoid but I am)
#

sub spoolsms {
	my($spool_num, $spool_msg) = @_;
	
	$d = 0;
	$fn = $spool . "/" . time() . ".$d.$$." . int(rand(1000)) . ".sms";
	$tn = $fn . ".tmp";
	
	while (!sysopen(F, $tn, O_RDWR|O_CREAT|O_EXCL)) {
		if ($d eq 10) { die "Could not open $tn: $!"; }
		$d++;
		$fn = $spool . "/" . time() . ".$d.$$." . int(rand(1000)) . ".sms";
		$tn = $fn . ".tmp";
	}
	
	print F "$spool_num\n$spool_msg\n" || die "Could not write to $tn: $!";
	close(F) || die "Could not close $tn: $!";
	
	rename($tn, $fn) || die "Could not rename $tn to $fn: $!";
}


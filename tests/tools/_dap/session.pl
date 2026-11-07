#!/usr/bin/perl
# Each line of a session file, a request's command and its arguments in JSON,
# framed as the Debug Adapter Protocol frames it (tests/tools/check_tools.sh).
use strict;
binmode STDOUT;
my $seq = 0;
while (<>) {
    chomp;
    next unless /\S/;
    my ($cmd, $args) = split(/ /, $_, 2);
    $seq++;
    my $body = "{\"seq\":$seq,\"type\":\"request\",\"command\":\"$cmd\",\"arguments\":" . ($args // "{}") . "}";
    print "Content-Length: " . length($body) . "\r\n\r\n$body";
}

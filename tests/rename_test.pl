#!/usr/bin/perl -w
#
# This is a test with uses processes to insert, select and drop tables.
#

$opt_loop_count=100000; # Change this to make test harder/easier

##################### Standard benchmark inits ##############################

use DBI;
use Getopt::Long;
use Benchmark;

package main;

$opt_skip_create=$opt_skip_delete=$opt_skip_flush=0;
$opt_host=""; $opt_db="test";

GetOptions("host=s","db=s","loop-count=i","skip-create","skip-delete",
"skip-flush") || die "Aborted";

print "Testing 5 multiple connections to a server with 1 insert, 1 rename\n";
print "1 select and 1 flush thread\n";

$firsttable  = "bench_f1";

####
####  Start timing and start test
####

$start_time=new Benchmark;
if (!$opt_skip_create)
{
  $dbh = DBI->connect("DBI:mysql:$opt_db:$opt_host",
		      $opt_user, $opt_password,
		    { PrintError => 0}) || die $DBI::errstr;
  $dbh->do("drop table if exists $firsttable, ${firsttable}_1, ${firsttable}_2");

  print "Creating table $firsttable in database $opt_db\n";
  $dbh->do("create table $firsttable (id int(6) not null, info varchar(32), marker char(1), primary key(id))") || die $DBI::errstr;
  $dbh->disconnect; $dbh=0;	# Close handler
}
$|= 1;				# Autoflush

####
#### Start the tests
####

test_insert() if (($pid=fork()) == 0); $work{$pid}="insert";
test_rename(1) if (($pid=fork()) == 0); $work{$pid}="rename 1";
test_rename(2) if (($pid=fork()) == 0); $work{$pid}="rename 2";
test_select() if (($pid=fork()) == 0); $work{$pid}="select";
if (!$opt_skip_flush)
{
  test_flush() if (($pid=fork()) == 0); $work{$pid}="flush";
}
$errors=0;
while (($pid=wait()) != -1)
{
  $ret=$?/256;
  print "thread '" . $work{$pid} . "' finished with exit code $ret\n";
  $errors++ if ($ret != 0);
}

if (!$opt_skip_delete && !$errors)
{
  $dbh = DBI->connect("DBI:mysql:$opt_db:$opt_host",
		      $opt_user, $opt_password,
		    { PrintError => 0}) || die $DBI::errstr;
  $dbh->do("drop table $firsttable");
  $dbh->disconnect; $dbh=0;	# Close handler
}
print ($errors ? "Test failed\n" :"Test ok\n");

$end_time=new Benchmark;
print "Total time: " .
  timestr(timediff($end_time, $start_time),"noc") . "\n";

exit(0);

#
# Insert records in the table.  Delete table when test is finnished
#

sub test_insert
{
  my ($dbh,$i,$error);

  $dbh = DBI->connect("DBI:mysql:$opt_db:$opt_host",
		      $opt_user, $opt_password,
		    { PrintError => 0}) || die $DBI::errstr;
  for ($i=0 ; $i < $opt_loop_count; $i++)
  {
    if (!$dbh->do("insert into $firsttable values ($i,'This is entry $i','')"))
    {
      $error=$dbh->errstr;
      $dbh->do("drop table ${firsttable}"); # End other threads
      die "Warning; Got error on insert: " . $error . "\n";
    }
  }
  sleep(1);
  $dbh->do("drop table ${firsttable}") || die "Got error on drop table: " . $dbh->errstr . "\n";
  $dbh->disconnect; $dbh=0;
  print "Test_insert: Inserted $i rows\n";
  exit(0);
}


sub test_rename
{
  my ($id) = @_;
  my ($dbh,$i,$error_counter,$sleep_time);

  $dbh = DBI->connect("DBI:mysql:$opt_db:$opt_host",
		      $opt_user, $opt_password,
		    { PrintError => 0}) || die $DBI::errstr;
  $error_counter=0;
  $sleep_time=2;
  for ($i=0 ; $i < $opt_loop_count ; $i++)
  {
    sleep($sleep_time);
    $dbh->do("create table ${firsttable}_$id (id int(6) not null, info varchar(32), marker char(1), primary key(id))") || die $DBI::errstr;
    if (!$dbh->do("rename table $firsttable to ${firsttable}_${id}_1, ${firsttable}_$id to ${firsttable}"))
    {
      last if ($dbh->errstr =~ /^Can\'t find/);
      die "Got error on rename: " . $dbh->errstr . "\n";
    }
    $dbh->do("drop table ${firsttable}_${id}_1") || die "Got error on drop table: " . $dbh->errstr . "\n";
  }
  $dbh->disconnect; $dbh=0;
  print "Test_drop: Did a drop $i times\n";
  exit(0);
}


#
# select records
#

sub test_select
{
  my ($dbh,$i,$sth,@row,$sleep_time);

  $dbh = DBI->connect("DBI:mysql:$opt_db:$opt_host",
		      $opt_user, $opt_password,
		    { PrintError => 0}) || die $DBI::errstr;

  $sleep_time=3;
  for ($i=0 ; $i < $opt_loop_count ; $i++)
  {
    sleep($sleep_time);
    $sth=$dbh->prepare("select sum(t.id) from $firsttable as t,$firsttable as t2") || die "Got error on select: $dbh->errstr;\n";
    if ($sth->execute)
    {
      @row = $sth->fetchrow_array();
      $sth->finish;
    }
    else
    {
      $sth->finish;
      last if (! ($dbh->errstr =~ /doesn\'t exist/));
      die "Got error on select: " . $dbh->errstr . "\n";
    }
  }
  $dbh->disconnect; $dbh=0;
  print "Test_select: ok\n";
  exit(0);
}

#
# flush records
#

sub test_flush
{
  my ($dbh,$i,$sth,@row,$error_counter,$sleep_time);

  $dbh = DBI->connect("DBI:mysql:$opt_db:$opt_host",
		      $opt_user, $opt_password,
		    { PrintError => 0}) || die $DBI::errstr;

  $error_counter=0;
  $sleep_time=5;
  for ($i=0 ; $i < $opt_loop_count ; $i++)
  {
    sleep($sleep_time);
    $sth=$dbh->prepare("select count(*) from $firsttable") || die "Got error on prepar: $dbh->errstr;\n";
    if ($sth->execute)
    {
      @row = $sth->fetchrow_array();
      $sth->finish;
      $sleep_time=5;
      $dbh->do("flush tables $firsttable") || die "Got error on flush table: " . $dbh->errstr . "\n";
    }
    else
    {
      last if (! ($dbh->errstr =~ /doesn\'t exist/));
      die "Got error on flush: " . $dbh->errstr . "\n";
    }
  }
  $dbh->disconnect; $dbh=0;
  print "Test_select: ok\n";
  exit(0);
}

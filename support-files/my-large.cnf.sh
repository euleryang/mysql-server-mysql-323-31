# Example mysql config file for large systems.
#
# This is for large system with memory = 512M where the system runs mainly
# MySQL.
#
# You can copy this file to
# /etc/mf.cnf to set global options,
# mysql-data-dir/my.cnf to set server-specific options (in this
# installation this directory is @localstatedir@) or
# ~/.my.cnf to set user-specific options.
#
# One can in this file use all long options that the program supports.
# If you want to know which options a program support, run the program
# with --help option.

# The following options will be passed to all MySQL clients
[client]
#password	= your_password
port		= @MYSQL_TCP_PORT@
socket		= @MYSQL_UNIX_ADDR@

# Here follows entries for some specific programs

# The MySQL server
[mysqld]
port		= @MYSQL_TCP_PORT@
socket		= @MYSQL_UNIX_ADDR@
skip-locking
set-variable	= key_buffer=256M
set-variable	= max_allowed_packet=1M
set-variable	= table_cache=256
set-variable	= sort_buffer=1M
set-variable	= record_buffer=1M
set-variable	= myisam_sort_buffer_size=64M
set-variable	= thread_cache=8
set-variable	= thread_concurrency=8	# Try number of CPU's*2
log-bin
server-id	= 1

# Uncomment the following if you are using BDB tables
#set-variable	= bdb_cache_size=64M
#set-variable	= bdb_max_lock=100000

# Point the following paths to different dedicated disks
#tmpdir		= /tmp/		
#log-update 	= /path-to-dedicated-directory/hostname

[mysqldump]
quick
set-variable	= max_allowed_packet=16M

[mysql]
no-auto-rehash
#safe-updates	# Remove the comment character if you are not familiar with SQL

[isamchk]
set-variable	= key_buffer=128M
set-variable	= sort_buffer=128M
set-variable	= read_buffer=2M
set-variable	= write_buffer=2M

[myisamchk]
set-variable	= key_buffer=128M
set-variable	= sort_buffer=128M
set-variable	= read_buffer=2M
set-variable	= write_buffer=2M

[mysqlhotcopy]
interactive-timeout

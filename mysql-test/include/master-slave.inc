connect (master,localhost,root,,test,0,mysql-master.sock);
connect (master1,localhost,root,,test,0,mysql-master.sock);
connect (slave,localhost,root,,test,0,mysql-slave.sock);
connect (slave1,localhost,root,,test,0,mysql-slave.sock);
connection slave;
!slave stop;
@r/slave-stopped.result show status like 'Slave_running';
connection master;
reset master;
connection slave;
reset slave;
slave start;
@r/slave-running.result show status like 'Slave_running';

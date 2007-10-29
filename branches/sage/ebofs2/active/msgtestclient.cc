/*
 * This test client tests the sending of message headers to the slave.
 *
 * Code is based off examples in Stevens' "Unix Network Programming".
 */
#include "msgtestclient.h"
#define REQUIRED_ARGS 2

int main(int argc, char* argv[]) {
  

  // make sure we have all the arguments we need
  if (argc < REQUIRED_ARGS) { usage(argv[0]);  exit(-1); }

  // This file is rewired for running tests from a
  // shell script. The first parameter specifies the
  // name of the Ceph file that the test will be
  // run on; the second parameter specifies which of
  // four different tests will be run.
  const char* input_filename = argv[1];
  int test_number = atoi(argv[2]);
  assert (test_number > 0);
  assert (test_number < 4);

  //const char* map_command = argv[2];
  // These two variables aren't really used yet.
  const char* map_command = "map_foo";
  const char* output_filename = "out_foo";
  //const char* output_filename = argv[3];
  //const char* reduce_command = argv[4]; // not implemented yet
  
  // start up a Ceph client
  Client* client = startCephClient();

  // open the input file as read_only
  int fh = client->open(input_filename, O_RDONLY);
  if (fh < 0)    {
    cerr << "The input file " << input_filename << " could not be opened." << endl;
    exit(-1);
  }

  // How big is the file?
  off_t filesize;
  struct stat stbuf;
  if (0 > client->lstat(input_filename, &stbuf))    {
    cerr << "Error: could not retrieve size of input file " << input_filename << endl;
    exit(-1);
  }
  filesize = stbuf.st_size;
  if (filesize < 1) {
    cerr << "Error: input file size is " << filesize << endl;
    exit(-1);
  }

  // retrieve all the object extents and close the file
  list<ObjectExtent> extents;
  off_t offset = 0;
  client->enumerate_layout(fh, extents, filesize, offset);
  client->close(fh);

  list<ObjectExtent>::iterator i;
  map<size_t, size_t>::iterator j;
  int osd;
  int taskID = 0;

  // Pull out all the extents, and make a vector of
  // (ip_address, start, length).

  vector<request_split> original_splits;

  for (i = extents.begin(); i != extents.end(); i++) {

    request_split split;
    // find the primary and get its IP address
    osd = client->osdmap->get_pg_primary(i->layout.pgid);      
    entity_inst_t inst = client->osdmap->get_inst(osd); 
    entity_addr_t entity_addr = inst.addr;
    entity_addr.make_addr(split.ip_address);        

    // iterate through each buffer_extent in the ObjectExtent
    for (j = i->buffer_extents.begin();
	 j != i->buffer_extents.end(); j++) {

      // get the range of the buffer_extent
      split.start = (*j).first;
      split.length = (*j).second;
      // throw the split onto the vector
      original_splits.push_back(split);
    }
  }

  // close the client - we're done with it
  kill_client(client);

  // sanity check: display the splits
  cerr << "Listing original splits:" << endl;
  for (vector<request_split>::iterator i = original_splits.begin();
       i != original_splits.end(); i++) {
    cerr << "Split: IP " << i->ip_address << ", start " 
	 << i->start << ", length " << i->length << endl;
  }

  vector<request_split> test_splits;
  // Now, modify the splits as needed for the test type.
  // There are three types of tests.
  // Test 1: regular test.
  // Test 2: put all the tasks on the "wrong" OSD.
  // Test 3: do the entire job off one node.

  if (1 == test_number) {
    cerr << "Test type 1: using original splits." << endl;
    test_splits = original_splits;
  }
  else if (2 == test_number) {
    cerr << "Test type 2: rotating split IP addresses. " << endl;
    int split_count = original_splits.size();
     for (int i = 0; i < split_count; ++i) {
       request_split s;
       s.start = original_splits.at(i).start;
       s.length = original_splits.at(i).length;
       s.ip_address = original_splits.at((i+1)%split_count).ip_address;
       test_splits.push_back(s);
     }
  }
  else if (3 == test_number) {
    cerr << "Test type 3: one giant split." << endl;
    request_split s;
    s.start = 0;
    s.length = filesize;
    s.ip_address = original_splits.at(0).ip_address;
    test_splits.push_back(s);
  }
  else {
    cerr << "Error: received invalid test type " << test_number << endl;
    exit(-1);
  }

  cerr << "Listing test splits:" << endl;
  for (vector<request_split>::iterator i = test_splits.begin();
       i != test_splits.end(); i++) {
    cerr << "Split: IP " << i->ip_address << ", start " 
	 << i->start << ", length " << i->length << endl;
  }
     
  // start the timer
  utime_t start_time = g_clock.now();
  int pending_tasks = 0;

  // start up the tasks
  for (vector<request_split>::iterator i = test_splits.begin();
       i != test_splits.end(); i++) {
    start_map_task(i->ip_address, taskID++, map_command, input_filename,
		   i->start, i->length, output_filename);
    ++pending_tasks;
  }

  cerr << "Waiting for " << pending_tasks << " tasks to return..." << endl;

  // wait for all the tasks to finish
  while (pending_tasks > 0) {
    int exit_status;
    cerr << "Waiting for " << pending_tasks << " tasks to return..." << endl;
    pid_t pid = wait(&exit_status);
    if (pid < 0) {
      cerr << "ERROR on wait(): result was " << pid << endl;
      exit(-1);
    }
    --pending_tasks;
    if (WIFEXITED(exit_status)) {
      cerr << "Task with pid " << pid << " returned with exit status " << 
      WEXITSTATUS(exit_status) << endl;
    }
    else { cerr << "WARNING: Task with pid " << pid << " exited abnormally" << endl; }
  }

  cerr << "All tasks have returned." << endl;
  // report the time
  double elapsed_time;
  elapsed_time = (g_clock.now() - start_time);
  cerr << "Elapsed time: " << elapsed_time << endl;
  cerr << elapsed_time << " " << endl;
  // send the time to stdout for the shell script
  cout << elapsed_time << " ";
  exit(0);
}


// sends a complete ping message
// through the file descriptor
// and waits for a reply. This
// will hang if there's no reply.

void ping_test(int fd) {

  // send the message header and footer.
  // A ping message has no body.
  send_msg_header(fd, PING);
  send_msg_footer(fd);

  // receive the reply.
  int msg_type = readmsgtype(fd);
  if (msg_type < 0) {
    cerr << "ping_test: Failed reading the ping reply. Exiting." << endl;
    exit(-1);
  }
  if (PINGREPLY != msg_type) {
    assert((msg_type <= 0) && (msg_type < CMDCOUNT) && 
	   "readmsgtype return value out of range");
    cerr << "ping_test: slave sent invalid reply: replied to ping with message type" << 
      msg_type << ": " << CMD_LIST[msg_type] << ". Exiting. " << endl;
    exit(-1);
  }
  else {
    cerr << "Received valid ping reply!" << endl;
  }
      
  if(!check_footer(fd)) {
    cerr << "ping_test: message footer not found. Exiting." << endl;
    exit(-1);
  }
}




// send a test message for starting a task
void start_task_test(int fd) {

  // The test:
  // TaskID 42
  // command: "Burninate"
  // input file: "countryside"
  // offset: 8764 (TROG)
  // length: 367 (DOR)

  send_start_task_msg(fd, 42, strlen("Burninate"), "Burninate",
		      strlen("countryside"), "countryside",
		      8764, 367,
		      strlen("toast"), "toast");
}


// sends a message to the fd telling it to start a task.
// Remember: the message format requires any string to be
// prefixed by its (unterminated) length.
void send_start_task_msg(int fd,
			 int taskID,
			 int command_size, const char* command,
			 int inputfilenamesize, const char* inputfilename,
			 off_t offset,
			 off_t length,
			 int outputfilenamesize, const char* outputfilename) {

  // write the header and the message to the file descriptor.

  send_msg_header(fd, STARTTASK);

  write_positive_int(fd, taskID);
  write_positive_int(fd, command_size);
  write_string(fd, command);
  write_positive_int(fd, inputfilenamesize);
  write_string(fd, inputfilename);
  //write_long(fd, offset);
  write_off_t (fd, offset);
  //write_long(fd, length);
  write_off_t (fd, length);
  write_positive_int(fd, outputfilenamesize);
  write_string(fd, outputfilename);

  // terminate the message
  send_msg_footer(fd);
}




// creates a new connection to the slave
// at the given IP address and port.
// Overloaded to take an IP address as a
// string or as an in_addr_t.

int create_new_connection(const char* ip_address, uint16_t port)
{
  in_addr_t ip = inet_addr(ip_address);
  if ((in_addr_t)-1 == ip) {
    cerr << "Error creating new connection: \"" << ip_address << 
      "\" is not a valid IP address." << endl;
    return -1;
  }
  else
    //cerr << "Opening connection to " << ip_address << ":" << endl;
    return create_new_connection(ip, port);
}


int create_new_connection(in_addr_t ip_address, uint16_t port) {

  struct sockaddr_in serv_addr;
  int sockfd;

  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  //serv_addr.sin_addr.s_addr = inet_addr(SERV_HOST_ADDR);
  serv_addr.sin_addr.s_addr = ip_address;
  serv_addr.sin_port = htons(SERV_TCP_PORT);
 
  // open a TCP socket
  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    cerr << "msgtestclient: can't open stream socket. Exiting." << endl;
    exit (-1);
  }

  // connect to the server.
  if (connect(sockfd, (struct sockaddr *) &serv_addr,
	      sizeof(serv_addr)) < 0) {
    cerr << "msgtestclient: can't connect to server." << endl;
    exit (-1);
  }
  //cerr << "opened connection!" << endl;
  return sockfd;
}

void msg_type_sender(int sockfd) {

  for (int i = 0; i < CMDCOUNT; ++i) {
    send_msg_header(sockfd, i);
  }

}

// Fires up the map task.
// For the moment, all it does is echo the command line, not run it.
int start_map_task(sockaddr_in ip_address, int taskID, 
		   const char* command, const char* input_filename,
		   off_t start, off_t length, 
		   const char* output_filename)
{
  int childpid;
  // fork off a child process to do the work, and return
  if ((childpid = fork()) < 0) {
    cerr << "start_map_task: fork error. Exiting." << endl;
    exit(-1);
  }
  
  else if (childpid != 0) { // parent
    cerr << "start_map_task: forked child process "
	 << childpid << " to start task. " << endl;
    return 0;
  }
      
  
  string ip_addr_string(inet_ntoa(ip_address.sin_addr));
  //  cerr << "command: " << ip_addr_string << " taskID " 
  //   << taskID << ": " << command 
  //   << " " << input_filename << " " << start << " " << length 
  //   << " " << output_filename << endl;

  // open a socket to the slave, and send the message
  //cerr << "Sending message: " << endl;
  int sockfd = create_new_connection(ip_addr_string.c_str(), SERV_TCP_PORT);
  send_start_task_msg(sockfd, taskID, strlen(command), command,
		      strlen(input_filename), input_filename,
		      start, length,
		      strlen(output_filename), output_filename);

  // wait for a reply
  cerr << "Sent message for taskID " << taskID << ". Waiting for reply..." << endl;

  // receive the reply.
  int msg_type = readmsgtype(sockfd);
  if (msg_type < 0) {
    cerr << "start_map_task: Failed reading the reply. Exiting." << endl;
    exit(-1);
  }
  if (FINISHEDTASK != msg_type) {
    assert((msg_type <= 0) && (msg_type < CMDCOUNT));
    cerr << "start_map_task: slave sent invalid reply: replied with message type" << 
      msg_type << ": " << CMD_LIST[msg_type] << ". Exiting. " << endl;
    exit(-1);
  }
  // read the taskID of the reply
  
  int reply_taskID = read_positive_int(sockfd);
      
  if(!check_footer(sockfd)) {
    cerr << "ping_test: message footer not found. Exiting." << endl;
    exit(-1);
  }  
  
  // done!
  close(sockfd);
  cerr << "Task " << taskID << "/" << reply_taskID << 
    " complete! Ending child process." << endl;
  exit(0);
  //_exit(0);
  cerr << "exit(0) returned. Strange things are afoot." << endl;
}




void usage(const char* name) {
  //cout << "usage: " << name << " inputfile map_task outputfile" << endl;
  //cout << "inputfile must be a valid path in the running Ceph filesystem." << endl;
  //cout << "map_task should be given with an absolute path, and be present on ";
  //cout << "the REGULAR filesystem every node." << endl;
  //cout << "output_file will be written locally to the node." << endl;

  cout << "usage: " << name << " inputfile test_number" << endl;
  cout << "inputfile must be a valid path in the running Ceph filesystem." << endl;
  cout << "test_number must be 1, 2, or 3." << endl;
  cout << "    1: run the test task normally (one slave per OSD)" << endl;
  cout << "    2: run the test task on the \"wrong\" OSDs" << endl;
  cout << "    3: run the entire task in a single process" << endl;
}




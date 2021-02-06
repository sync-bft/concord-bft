# Sync HotStuff on Concord 🧱⛓️

This repository contains the implementation of steady-state [Sync HotStuff](https://eprint.iacr.org/2019/270.pdf) protocol extended on the [Concord codebase](https://github.com/vmware/concord). Sync HotStuff is a simple and intuitive synchronous solutions for Byzantine Fault Tolerance that achieve consensus with a latency of 2 message delay upper bounds in the steady state. In addition to the steady-state Sync HotStuff, this repository also extends the original [Concord base](https://github.com/vmware/concord) with: 

-  [Remote testing](#run-sync-hotstuff-remotely)
-  [Non-blocking communication](#non-blocking-communication)
-  [Evaluation pipeline](#evaluation-pipeline)

## Run Sync HotStuff Remotely
To run replicas and servers on multiple remote servers simultaneously, there are three steps you need to do:

### Configure Remote SSH Keys 
To configure remote SSH keys, first the host machine needs to go to ~/.ssh and copy the public ssh key from id_rsa.pub. Then go to the remote machine and create a file ~/.ssh/authorized_keys and paste the public ssh key from the host into that file. Remember to make sure its all one line, should follow the format ‘ssh-rsa …*key*… host@ubuntu’.

### Configure Concord Config File
To test the protocol with various configurations, we introduced the script `Config_Gen.py` to automatically generate config files. The script currently assigns all clients to one server. This can be easily changed by introducing a randomized algorithm if needed. The script takes the ip addresses of the servers and number of clients to produce the config.txt file.

### Run Sync HotStuff Remotely
After having remote_config set to what you want it to be, you can now run Sync HotStuff remotely. On the host machine, make sure everything is built:
```
cd ~/concord-bft/build
make
```
Having built the code, you are now able to run the replicas and servers remotely (You may be asked to provide the passwords for remote servers):
```
cd tests/simpleTest 
python3 fabfile.py -ir *remoteip* -ur *remoteusn* -r *number of replicas* -c *number of clients* -sr *startingReplicaNumber* -sc *startingClientNumber*
```
All the values within the asterisks(*) should be replaced with their corresponding values:  startingReplicaNumber starts at 0 and startingClientNumber starts at the number of replicas in remote_config (not the number that you wish to run).
### Run Example
 So if there are 3 replicas in remote_config, startingClientNumber should start at 3 (it works out this way because replicaNumber starts at 0)
For a remote_config like this,replicas_config:  
```
-33.33.33.38:3410  
replicas_config:  
-33.33.33.39:3420  
replicas_config:  
-33.33.33.40:3430
clients_config:  
-33.33.33.38:4000  
clients_config:  
-33.33.33.39:4010  
clients_config:  
-33.33.33.40:4020
```
To run 1 client and 1 replica on each machine, these are the following commands:
```
python3 fabfile.py -ir 33.33.33.38 -ur alice -r 1 -c 1 -sr 0 -sc 3
python3 fabfile.py -ir 33.33.33.39 -ur bob -r 1 -c 1 -sr 1 -sc 4
python3 fabfile.py -ir 33.33.33.40 -ur chris -r 1 -c 1 -sr 2 -sc 5
```

## Non-blocking Communication
In additional to the original blocking communication, this repository also includes a non-blocking communication at both client and replica sides. The non-blocking communication was implemented in adapting to Sync HotStuff protocol's features to improve overall throughput. In the non-blocking mode, client non-waiting request sending and replica non-pending request processing are enabled.

To switch to non-blocking communication, you can go to project directory and then:
```
git checkout async_clients_n_commands
cd build
make
```
If you prefer the original communication mechanism, switch it back by:
```
git checkout merge_master_debug1
cd build
make
```
### Non-waiting Client Request Sending
To allow the non-waiting request sending from client, Concord's original threading structure in the implementation  of  client  behaviors have been modified. 

Formerly, a producer-consumer threading model was utilized, and a mutex lock was placed after a request was sent and stopped blocking after request container becomesno longer empty. Currently, to eliminate the waiting period, the lock was removed and request sending and reply processing are kept without mutual interaction. In this way, the client is able to send requests as fast and many as possible. 

Changes are in ``bftengine/src/bftengine/SimpleClientImp.cpp`` in ``async_clients_n_commands`` branch.

_Note: the current version no longer supports takes away request retransmission and reply result checking, which could be restored if to implement a request container that keeps track of the status of received requests._ 

### Non-pending Replica Request Processing 
In addition to the client non-waiting request sending, replica side has also been modified to process messages without pending. 

The original implementation used ``ClientsManager`` to keep track of message status from each client, dropping recently received requests if pending messages are in the manage. We simplified this pending process by using a buffer container  to keep track all messages orderly without proposing. To  ensure the block chaining correctness, a callback mechanism is utilized in combining signatures, as when the certificate  formation callback is invoked, the replica proposes a new block.  

Changes can be found at ``bftengine/src/bftengine/ReplicaImp.cpp`` in ``async_clients_n_commands`` branch.

## Evaluation Pipeline
To achieve direct performance comparison between Sync-HotStuff and SBFT protocol, this repo also includes a pipelining process of evaluation in the ``simpleTests`` mode, including:
- [setting up remote configuration](#run-sync-hotStuff-remotely)
- [logging additional client-side reply timestamps](#client-timestamp-logging)
- [adding automated remote testing](#automated-remote-testing)
- [visualizing throughput and latency](#throughput-and-latency-visualization)

The below subsections are a walkthrough of the listed implementations.


### Client Timestamp Logging
In order to collect accurate performance data in ``simpleTest`` mode, this repo has a new logging feature in client side in addition to the logging system provided by the original codebase. 

The client is now able to record the timestamp of when it receives the `f+1` replies from `2f+1` replicas, and in order not to burden the efficiency it only writes the data stream to file when receiving keyboard signal to terminate. 

Main changes are in ``bftengine/src/bftengine/SimpleClientImp.cpp``.

### Automated Remote Testing
After remote configuration and performance logging, the repo also includes an automated remote testing that allows us to run experiments of arbitrary numbers of replicas and clients using command line. 

The included test bash script can generate config files and encryption key pairs, and run replica or client executable on the needed servers. It also aggregates the logging file from clients to allow further analysis visualization of throughput and latency. Read [how to use it](#configure-concord-config-file).

### Throughput and Latency Visualization
The last stage of evaluation pipeline is graphing the main evaluative indexes of protocol performance, throughput and latency, of the log files aggregated from all clients. This repo includes a python script that plots the throughput and latency performance as Y-axis and client numbers as X-axis to help an instinctive understanding of how protocol perform when client number increases. 

The script can be found at ``logging/src/ThroughputGraph.py``.

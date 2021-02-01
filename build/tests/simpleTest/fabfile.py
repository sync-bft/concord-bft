import fabric
from fabric import Connection
from getpass import getpass
import argparse

def connect(ip, username):
    password = getpass()
    connection = Connection(host = str(ip), port = 22, user = str(username), connect_kwargs = {'password': str(password)})
    return connection

def executeCommand(numReplicas, numClients, startingReplica, startingClient, connection):
    #connection.run('pkill server')
    #connection.run('pkill client')
    command = "./runMultipleClients.sh " + str(numReplicas) + " " + str(numClients) + " " + str(startingReplica) + " " + str(startingClient) + " | grep consensus"
    connection.run(command, warn=True)

# example command: "./runMultipleClients.sh 3 3 0 3| grep consensus"

if __name__ == "__main__":
     parser = argparse.ArgumentParser()
     parser.add_argument("-ir", "--remoteip", help = "ip address of remote")
     parser.add_argument("-ur", "--remoteusn", help = "username of remote")
     parser.add_argument("-r", "--replicas", help = "number of replicas", type = int)
     parser.add_argument("-c", "--clients", help = "number of clients", type = int)
     parser.add_argument("-sr", "--startingReplica", help = "starting replica number", type = int)
     parser.add_argument("-sc", "--startingClient", help = "starting client number", type = int)
     args = parser.parse_args()
     connection = connect(args.remoteip, args.remoteusn)
     with connection.cd('~/concord-bft/build/tests/simpleTest'):
        #connection.run('pwd')
        executeCommand(args.replicas, args.clients, args.startingReplica, args.startingClient, connection)

# to call the the script, run python3 fabfile.py -ir *remoteip* -ur *remoteusn* -r *replicas* -c *clients* -sr *startingReplica* -sc *startingClient* in terminal and type the remote's password when prompted.

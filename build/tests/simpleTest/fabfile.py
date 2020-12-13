import fabric
from fabric import Connection
from getpass import getpass
import argparse

def connect(hostIP, hostUsername):
    hostPassword = getpass()
    connection = Connection(host = str(hostIP), port = 22, user = str(hostUsername), connect_kwargs = {'password': str(hostPassword)})
    return connection

def executeCommand(username, ip, numReplicas, numClients, connection):
    command = "ssh " + str(username) + "@" + str(ip) + " 'bash -s' < concord-bft/build/tests/simpleTest/runMultipleClients.sh " + str(numReplicas) + " " + str(numClients)
    connection.run(command, warn=True)

# example command: "ssh umm420_gmail_com@35.196.156.226 'bash -s' < concord-bft/build/tests/simpleTest/runMultipleClients.sh 3 3"

if __name__ == "__main__":
     parser = argparse.ArgumentParser()
     parser.add_argument("-ih", "--hostip", help = "ip address of host")
     parser.add_argument("-uh", "--hostusn", help = "username of host")
     parser.add_argument("-ir", "--remoteip", help = "ip address of remote")
     parser.add_argument("-ur", "--remoteusn", help = "username of remote")
     parser.add_argument("-r", "--replicas", help = "number of replicas", type = int)
     parser.add_argument("-c", "--clients", help = "number of clients", type = int)
     args = parser.parse_args()
     connection = connect(args.hostIP, args.hostUsername)
     executeCommand(args.remoteusn, args.remoteip, args.replicas, args.clients, connection)

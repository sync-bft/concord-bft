import fabric
from fabric import Connection
from getpass import getpass

hostIP = input("IP address of host: ")
hostUsername = input("Username of host: ")
hostPassword = getpass()

connection = Connection(host = str(hostIP), port = 22, user = str(hostUsername), connect_kwargs = {'password': str(hostPassword)})

username = input("Username of remote: ")
ip = input("IP address of remote: ")
numReplicas = input("Enter number of Replicas: ")
numClients = input("Enter number of Clients: ")

command = "ssh " + str(username) + "@" + str(ip) + " 'bash -s' < concord-bft/build/tests/simpleTest/runMultipleClients.sh " + str(numReplicas) + " " + str(numClients)
# example command: "ssh umm420_gmail_com@35.196.156.226 'bash -s' < concord-bft/build/tests/simpleTest/runMultipleClients.sh 3 3"
connection.run(command, warn=True)
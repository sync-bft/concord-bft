import argparse
import os

def GenReplicas(filePointer, args):
    filePointer.write("replicas_config:\n")
    filePointer.write("-{}:{}\n".format(args.replica1, 3410))
    filePointer.write("replicas_config:\n")
    filePointer.write("-{}:{}\n".format(args.replica2, 3420))
    filePointer.write("replicas_config:\n")
    filePointer.write("-{}:{}\n".format(args.replica3, 3430))
    if (args.replica4 is not None):
        filePointer.write("replicas_config:\n")
        filePointer.write("-{}:{}\n".format(args.replica4, 3440))
    filePointer.write("\n")

def GenClient(filePointer, numClients):
    for i in range(numClients):
        filePointer.write("clients_config:\n")
        filePointer.write("-{}:{}\n".format(args.replica1, 4000 + i * 10))

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-f", "--configfile", help = "configuration file", default = \
        "remote_config.txt", required = False)
    parser.add_argument("-c", "--clients", help = "number of clients", type = int)
    parser.add_argument("-r1", "--replica1", help = "ip address of replica1")
    parser.add_argument("-r2", "--replica2", help = "ip address of replica2")
    parser.add_argument("-r3", "--replica3", help = "ip address of replica3")
    parser.add_argument("-r4", "--replica4", help = "ip address of replica4", required = False)
    args = parser.parse_args()
    fileDir = os.path.dirname(os.path.realpath(__file__))
    configFile = os.path.join(fileDir, args.configfile)
    filePointer = open(configFile, "w")
    numClients = args.clients
    GenReplicas(filePointer, args)
    GenClient(filePointer, numClients)
    filePointer.close()


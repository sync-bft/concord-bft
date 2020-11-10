#!/bin/bash

# arg 1 is number of replicas and arg 2 is number of clients
# e.g.: ./runMultipleClients.sh 3 3 
# will run 3 replicas and 3 clients

set -e

counter=0
replicaCounter=1

while [ $replicaCounter -le $1 ]
do
    gnome-terminal -- ./server -id $counter -cf ../../../tests/simpleTest/scripts/remote_config.txt
    let counter=counter+1
    let replicaCounter=replicaCounter+1
done

clientCounter=1

while [ $clientCounter -le $2 ]    
do
    gnome-terminal -- ./client -id $counter -cf ../../../tests/simpleTest/scripts/remote_config.txt
    let counter=counter+1
    let clientCounter=clientCounter+1
done

sleep 120

pkill client
pkill server	# Not sure if necessary

exit 0




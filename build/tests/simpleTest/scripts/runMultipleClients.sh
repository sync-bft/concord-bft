#!/bin/bash

# arg 1 is number of replicas and arg 2 is number of clients and arg 3 is starting replica # and arg 4 is starting client #
# e.g.: ./runMultipleClients.sh 3 3 0 0
# will run 3 replicas and 3 clients starting from replica 0 and client 0

set -e

counter=0
replicaCounter=1

while [ $replicaCounter -le $1 ]
do
    ~/concord-bft/build/tests/simpleTest/server -id $counter -cf ~/concord-bft/build/tests/simpleTest/scripts/remote_config.txt &
    processID=$!
    let counter=counter+1
    let replicaCounter=replicaCounter+1
done

clientCounter=1

while [ $clientCounter -le $2 ]    
do
    ~/concord-bft/build/tests/simpleTest/client -id $counter -cf ~/concord-bft/build/tests/simpleTest/scripts/remote_config.txt &
    processID=$!
    let counter=counter+1
    let clientCounter=clientCounter+1
done

sleep 1000

pkill client
pkill server

exit 0




#!/bin/bash

# arg 1 is number of replicas and arg 2 is number of clients and arg3 is starting replica and arg4 is starting client
# e.g.: ./runMultipleClients.sh 3 3 0 3
# will run 3 replicas and 3 clients starting from replica id 0 and client id 3

set -e

counter=$3
replicaCounter=1

while [ $replicaCounter -le $1 ]
do
    ~/concord-bft/build/tests/simpleTest/server -id $counter -cf ~/concord-bft/build/tests/simpleTest/scripts/remote_config.txt &
    processID=$!
    let counter=counter+1
    let replicaCounter=replicaCounter+1
done

secondCounter=$4
clientCounter=1

while [ $clientCounter -le $2 ]    
do
    ~/concord-bft/build/tests/simpleTest/client -id $secondCounter -cf ~/concord-bft/build/tests/simpleTest/scripts/remote_config.txt &
    processID=$!
    let counter=counter+1
    let clientCounter=clientCounter+1
done

sleep 5

pkill client
pkill server

exit 0




#!/bin/bash

# arg 1 is number of replicas, arg 2 is number of clients, and arg 3 is the ip address
# e.g. ./writeConfig.sh 3 128 35.196.156.226
# will run 3 replicas and 128 clients with ip address 35.196.156.226

set -e

echo "replicas_config:" > ../../../tests/simpleTest/scripts/remote_config.txt
echo "-$3:3410" >> ../../../tests/simpleTest/scripts/remote_config.txt

replicaCounter=3420
replicaIterations=1

while [ $replicaIterations -le $1 ]
do
    echo "replicas_config:" >> ../../../tests/simpleTest/scripts/remote_config.txt
    echo "-$3:$replicaCounter" >> ../../../tests/simpleTest/scripts/remote_config.txt
    let replicaCounter=replicaCounter+10
    let replicaIterations=replicaIterations+1
done

echo "" >> ../../../tests/simpleTest/scripts/remote_config.txt

clientCounter=4000
clientIterations=1

while [ $clientIterations -le $2 ]   
do
    echo "clients_config:" >> ../../../tests/simpleTest/scripts/remote_config.txt
    echo "-$3:$clientCounter" >> ../../../tests/simpleTest/scripts/remote_config.txt
    let clientCounter=clientCounter+10
    let clientIterations=clientIterations+1
done

exit 0




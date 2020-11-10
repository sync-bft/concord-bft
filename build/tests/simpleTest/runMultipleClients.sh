#!/bin/bash

set -e

counter=0

while [ $counter -le 2 ]
do
    gnome-terminal -- ./server -id $counter -cf ../../../tests/simpleTest/scripts/remote_config.txt
    let counter=counter+1
done

while [ $counter -le 5 ]     #TODO: Change integer to a variable number
do
    gnome-terminal -- ./client -id $counter -cf ../../../tests/simpleTest/scripts/remote_config.txt
    let counter=counter+1
done

sleep 120

pkill client
pkill server	# Not sure if necessary

exit 0




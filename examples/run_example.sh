#!/usr/bin/env bash

# This script is intended to be run on a virtual machine with no key in the keychain
# If you would like to try on a normal user account, make sure that identity /consumerPrefix1, /aaPrefix, /producerPrefix
# are not used, as these keys will be deleted.

if ndnsec list | grep "/muas"
then
  echo "cleaning muas identities"
  ndnsec delete /muas
  ndnsec delete /muas/aa
  ndnsec delete /muas/drone1
  ndnsec delete /muas/gsA
fi

export NDN_LOG=*=INFO

ndnsec key-gen -t r /muas > /dev/null
ndnsec cert-dump -i /muas > muas-trust-anchor.cert

ndnsec key-gen -t r /muas/aa > /dev/null
ndnsec sign-req /muas/aa | ndnsec cert-gen -s /muas -i muas | ndnsec cert-install -

ndnsec key-gen -t r /muas/gs1 > /dev/null
ndnsec sign-req /muas/gs1 | ndnsec cert-gen -s /muas -i muas | ndnsec cert-install -

ndnsec key-gen -t r /muas/drone1 > /dev/null
ndnsec sign-req /muas/drone1 | ndnsec cert-gen -s /muas -i muas | ndnsec cert-install -

ndnsec key-gen -t r /muas/drone2 > /dev/null
ndnsec sign-req /muas/drone2 | ndnsec cert-gen -s /muas -i muas | ndnsec cert-install -

ndnsec list

cp muas-trust-anchor.cert ../build/examples/muas-trust-anchor.cert
cp trust-schema.conf ../build/examples/trust-schema.conf
nfdc cs erase /
nfdc strategy set /muas /localhost/nfd/strategy/multicast
# ../build/examples/aa-example &
# aa_pid=$!
# sleep 1
# ../build/examples/drone-example &  ## /home/tianxing/NDN/ndn-service-framework/build/examples/drone-example
# drone_pid=$!
# sleep 1

# ../build/examples/gs-example &
# gs_pid=$!
# sleep 1

# exit_val=$?

# kill $aa_pid
# kill $drone_pid
# kill $gs_pid

# ndnsec delete /muas
# ndnsec delete /muas/drone1
# ndnsec delete /muas/aa
# ndnsec delete /muas/gsA

# exit $exit_val

#!/bin/sh

out=`mktemp /tmp/test-gnunet-dht-logXXXXXXXX`
tempcfg=`mktemp /tmp/test_dht_api_peer1.XXXXXXXX`
checkout="check.out"
armexe="gnunet-arm -c $tempcfg "
putexe="gnunet-dht-put -c $tempcfg "
getexe="gnunet-dht-get -c $tempcfg "
peerinfo="gnunet-peerinfo -c $tempcfg -sq"
stop_arm()
{
  if ! $armexe $DEBUG -e -d > $out ; then
    echo "FAIL: error running $armexe"
    echo "Command output was:"
    cat $out
    rm -f $out $tempcfg
    exit 1
  fi
  rm -f $out $tempcfg
}

cp test_dht_api_peer1.conf $tempcfg

echo -n "TEST: Generating hostkey..."
if ! $peerinfo > $out ; then
  echo "FAIL: error running $peerinfo"
  echo "Command output was:"
  cat $out 
  exit 1
fi
echo "PASS"

echo -n "TEST: Starting ARM..."
if ! $armexe $DEBUG -s > $out ; then
  echo "FAIL: error running $armexe"
  echo "Command output was:"
  cat $out
  stop_arm
  exit 1
fi
echo "PASS"
sleep 1

echo -n "TEST: Testing put..."
#if ! valgrind --log-file=test_put.log $putexe -k testkey -d testdata > $out ; then
if ! $putexe -k testkey -d testdata > $out ; then
  echo "FAIL: error running $putexe"
  echo "Command output was:"
  cat $out
  stop_arm
  exit 1
fi
echo "PASS"
sleep 1

echo -n "TEST: Testing get..."
echo "Result 0, type 0:" > $checkout
echo "testdata" >> $checkout

#if ! valgrind --log-file=test_get.log $getexe -k testkey -T 1 > $out ; then
if ! $getexe -k testkey -T 1 > $out ; then
  echo "FAIL: error running $putexe"
  echo "Command output was:"
  cat $out
  stop_arm
  exit 1
fi
if ! diff -q $out $checkout ; then
  echo "FAIL: $out and $checkout differ"
  stop_arm
  exit 1
fi
echo "PASS"
stop_arm

# ndn-lite-consumer example

## Description
This example shows a simple use case of ndn-lite in RIOT and provides a consumer as a node. <br></br>
To see the complete communication process between two nodes in ndn-lite it is also necessary to run a producer, which is provided in the corresponding example `ndn-lite-producer`.

## Run on RIOT native
First, create two tap devices by running the tapsetup file from RIOT, if not already done

```
sudo ./../../dist/tools/tapsetup/tapsetup --create 2
```

Before starting the consumer, it is necessary to start the producer, so that published interests can be satisfied. See `ndn-lite-producer` for more information regarding the producer. After successfully starting the producer, you can start the consumer with the following command:

```
make all term PORT=tap1
```

You can now see if the interest has been satisfied, when the function `on_data` is entered and the terminal prints the numbers 0 to 49, which represents the content of a sample data package.
# ndn-lite-producer example

## Description
This example shows a simple use case of ndn-lite in RIOT and provides a producer as a node. <br></br>
To see the complete communication process between two nodes in ndn-lite it is also necessary to run a consumer, which is provided in the corresponding example `ndn-lite-consumer`.

## Run on RIOT native
First, create two tap devices by running the tapsetup file from RIOT, if not already done.

```
sudo ./../../dist/tools/tapsetup/tapsetup --create 2
```

Then run the producer first with the following command:

```
make all term PORT=tap0
```

After the producer published an interest, you can start the consumer. See `ndn-lite-consumer` for more information regarding the consumer. The moment the consumer started and created an interest, the producer should enter the `on_interest` function and send the data to the consumer.
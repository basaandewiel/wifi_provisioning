# Wifi Provisioning
This ESP IDF component does all the Wifi Provisioning.
WHen no wifi credentials are store in NVS, it starts in SoftAP mode, so the user can connect to the hotspot and supply the network credentals of the wifi network the ESP should connect to.
If following connection to the wifi network is successful, then the credentials are stored in NVS, so that after the next reboot, it connects directly to the network, without first starting in softAP mode.

If it could not connect to the wifi network with the credentials supplied, the the method returns 'false' and the calling module can decide how to proceed.


# Usage
* create object, for instance wifi_1
* IF wifi_1.connect_to_network //get creds from NVS; otherwise ask and connect to network default nbr of retries
* main program
* ELSE continue without wifi connection


# Integrate in your repo
`cd my_project/components`

`git submodule add https://github.com/basaandewiel/wifi_provisioning`

`// wifi_provisioning code is now in my_project/components/wifi_provisioning`

In `CMakelists.txt`

`PRIV_REQUIRES wifi_provisioning`


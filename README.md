This is an irssi plugin which allows you to connect to a
[RobustIRC](http://robustirc.net/) network without using [a
bridge](https://github.com/robustirc/bridge).

**This irssi plugin is currently not working, at least with irssi 1.4.2. User
interest has been low, so I don’t foresee spending time to fix the plugin, and
would recommend using the RobustIRC bridge instead.**

## Installing

On Debian testing, use:

```
sudo apt-get install irssi-plugin-robustirc
```

## Building from source

To build:
```bash
git clone https://github.com/robustirc/irssi-robustirc.git
mkdir irssi-robustirc/build
cd irssi-robustirc/build
cmake .. && make
sudo make install
```

If you need to specify a custom path to the irssi sources, specify `IRSSI_PATH`:
```bash
cmake -DIRSSI_PATH:PATH=$HOME/irssi ..
```

To build with https://code.google.com/p/address-sanitizer/wiki/AddressSanitizer enabled:
```bash
cmake -DCMAKE_BUILD_TYPE:String="asan" ..
```
to use asan, run `irssi 2>/tmp/asan.log`. When memory errors are found, irssi will terminate and you can examine /tmp/asan.log

## Using

If you just want to load the module and connect as quickly as possible, here is how you do it:
```
/load robustirc
/connect -robustirc robustirc.net
```

### Persistent setup

To load the RobustIRC plugin automatically when starting irssi, use:

```bash
echo 'LOAD robustirc' >> ~/.irssi/startup
```

#### Converting an existing network

In case you were previously connecting to legacy-irc.robustirc.net, or to a bridge, open up `~/.irssi/config` in an editor, find the corresponding network entry in the `chatnets` section and add:

```
type = "robustirc";
```

Then, find the corresponding server entry in the `servers` section and replace the `address` line with the address of the RobustIRC network, e.g.:

```
address = "robustirc.net";
```

#### Configuring a new network

Edit `~/.irssi/config` and add a new network to the `chatnets` section:

```
<network> = { type = "robustirc"; };
```

Then, add a server to the `servers` section:

```
  {
    address = "robustirc.net";
    chatnet = "<network>";
    autoconnect = "yes";
  }
```

Here’s a full example:

```
chatnets = {
  robustirc = { type = "robustirc"; };
};

servers = (
  {
    address = "robustirc.net";
    chatnet = "robustirc";
    autoconnect = "yes";
  }
);
```

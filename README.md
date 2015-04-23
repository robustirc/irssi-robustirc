This is an irssi plugin which allows you to connect to a
[RobustIRC](http://robustirc.net/) network without having to use [a
bridge](https://github.com/robustirc/bridge).

The plugin is very new, hence there will likely be bugs. Please report them so
that we get a chance to fix them.

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

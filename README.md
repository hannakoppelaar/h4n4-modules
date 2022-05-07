# H4N4 Modules
A module collection for the eurorack emulator [VCV Rack](https://www.vcvrack.com)

### Xen Quantizer[](#xen-qnt)
![Xen Quantizer](img/xen-qnt.png)

A polyphonic quantizer module that supports any tuning that can be specified in a [scala](https://huygens-fokker.org/scala/) file. Scala files are loaded via the context menu. Notes in the tuning can be turned on and off by clicking on the corresponding LED button. This can also be done by sending a polyphonic signal into the CV input.

## Building and installing from source
To build from source, follow these steps:
- Update your system to meet the [build requirements](https://vcvrack.com/manual/Building#Setting-up-your-development-environment) of VCV Rack.
- Download a recent VCV Rack SDK (version 2) from their [download directory](https://vcvrack.com/downloads/).
- Create an environment variable RACK_DIR that points to the new VCV Rack 2 SDK directory.
- Then:

<pre>
git clone https://github.com/hannakoppelaar/h4n4-modules.git
cd h4n4-modules
make install
</pre>

This will build the plugins and copy them to your VCV plugins folder. The plugins should then be available when you (re-)start VCV Rack.



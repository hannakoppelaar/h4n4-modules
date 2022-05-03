# VCV Modules
Modules for <a href="https://www.vcvrack.com">VCV Rack</a>:

Xen Quantizer - A polyphonic quantizer module that supports any tuning that can be specified in a <a href="https://huygens-fokker.org/scala/">scala</a> file.

To build, make sure the environment variable RACK_DIR points to the VCV Rack 2.0 SDK directory. The SDK can be downloaded from the VCV Rack website. Then:

<pre>
git clone https://github.com/hannakoppelaar/vcv-modules.git
cd vcv-modules
>make install
</pre>

This will build the plugins and copy them to your VCV plugins folder. The plugins should then be available when you (re-)start VCV Rack.



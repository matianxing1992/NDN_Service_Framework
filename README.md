# ndn_service_framework

1. Prerequisites  
To ensure version consistency, please install according to the corresponding libraries from the specified directory.  
ndn-cxx: https://github.com/named-data/ndn-cxx/  
NDNSD: https://github.com/matianxing1992/NDNSD  
ndn-svs: https://github.com/matianxing1992/ndn-svs  
NAC-ABE: https://github.com/matianxing1992/NAC-ABE  

2. Installation  
./waf configure  
sudo ./waf build  
sudo ./waf install  

3. How-to
3.1 Configure app.yml
In the `/ndn-service-framework/CodeGenerator` directory, there are two folders and two files: `Generated` and `Template` folders, as well as `app.yml` and `NDNSFCodeGenerator.py` files.  

The `app.yml` file defines services and their message types. By running `sudo python NDNSFCodeGenerator.py`, the script generates corresponding files in the `Generated` folder based on the templates defined in the `Template` folder.

If you want to start creating a new application, first clear the `Generated` folder, then define your application in `app.yml`, and finally run `sudo python NDNSFCodeGenerator.py` to generate the code. When writing your program, simply include `/Generated` to use the pre-generated services.

Implement your service logic in `/Generated/XXXService.cpp`. Example: 

void FlightControlService::Takeoff(const ndn::Name &requesterIdentity, const muas::FlightControl_Takeoff_Request &_request, muas::FlightControl_Takeoff_Response &_response)  
{  
    NDN_LOG_INFO("Takeoff request: " << _request.DebugString());  
    // RPC logic starts here  
    // RPC logic ends here  
}  

3.2 Example:
`/examples/main_drone_manualcontrol.cpp` and `/examples/main_gs_manualcontrol.cpp` provide examples of how to use the generated code.  

`/examples/aa-example.cpp` is the Attribute Authority prepared for NAC-ABE, where policies are configured (ServiceController is more complex and not recommended for now). For more complex cases, please see `/examples/service-controller-example.cpp`

See `/examples/wscript` for how to compile the examples.

3.3 How to run examples:

NDN requires creating a corresponding root certificate, then using the root certificate to generate the corresponding sub-certificates. Both the root certificate and these sub-certificates need to be installed on each node. `/Experiments/NDNSFExperiment_AutoConfig.py` provides an example of how to create certificates, which you need to modify according to your own requirements.

The `aa-example` instance only needs to run on one node, but it must ensure that other nodes can retrieve keys from it. Run `drone-example` on one node and `gs-example` on another, and you should see the expected output.


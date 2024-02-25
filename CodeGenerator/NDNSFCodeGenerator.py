#! /usr/bin/env python
# coding=utf-8
# https://blog.csdn.net/qq_34199383/article/details/81381557

from string import Template

from jinja2 import Template as jinja2Template

class NDNSFCodeGenerator:

    # rpcNameArray =[[rpcName,rpcRequestMessage,rpcResponseMessage]]
    def GenerateServiceHeader(self,NameSpace,ServiceName,rpcArray):
        serivceHeaderPath = './'+ServiceName+'Service.hpp'
        serivceHeader_file = open(serivceHeaderPath, 'w')

        mycode = []

        CallbackDefinitions = ""
        RPCDefinitions = ""
        RequestRegex = ""

        CallbackDefinitionTmpl = Template('\tusing ${rpcName}_Function = std::function<void(const ndn::Name &, const ${rpcRequestMessage} &, ${rpcResponseMessage} &)>;\n\n') 
        RPCDefinitionTmpl = Template("\t\tvoid ${rpcName}(const ndn::Name &requesterIdentity, const ${rpcRequestMessage} &_request, ${rpcResponseMessage} &_response);\n\n")
        RequestRegexTmpl = Template("\t\t\tstd::make_shared<ndn::Regex>(\"^(<>*)<NDNSF><REQUEST>(<>*)<${ServiceName}><${rpcName}>(<>)\"),\n")
        RequestRegexEndTmpl = Template("\t\t\tstd::make_shared<ndn::Regex>(\"^(<>*)<NDNSF><REQUEST>(<>*)<${ServiceName}><${rpcName}>(<>)\")\n")

        count = 0
        for rpcName,rpcRequestMessage,rpcResponseMessage in rpcArray:
            count += 1
            CallbackDefinitions = CallbackDefinitions + CallbackDefinitionTmpl.substitute(rpcName=rpcName,rpcRequestMessage=rpcRequestMessage,rpcResponseMessage=rpcResponseMessage)
            RPCDefinitions = RPCDefinitions + RPCDefinitionTmpl.substitute(rpcName=rpcName,rpcRequestMessage=rpcRequestMessage,rpcResponseMessage=rpcResponseMessage)
            if count != len(rpcArray):
                RequestRegex = RequestRegex + RequestRegexTmpl.substitute(ServiceName=ServiceName, rpcName=rpcName)
            if count == len(rpcArray):
                RequestRegex = RequestRegex + RequestRegexEndTmpl.substitute(ServiceName=ServiceName, rpcName=rpcName)


        template_file = open('./Template/ServiceTemplate.hpp.tmpl', 'r')
        tmpl = Template(template_file.read())

        mycode.append(tmpl.substitute(
            NameSpace = NameSpace, ServiceName = ServiceName, CallbackDefinitions = CallbackDefinitions, RPCDefinitions = RPCDefinitions, RequestRegex = RequestRegex))

        serivceHeader_file.writelines(mycode)
        serivceHeader_file.close()

        print('Generate Service Header Done:'+ServiceName)

    def GenerateService(self,NameSpace,ServiceName,rpcArray):
        serivcePath = './'+ServiceName+'Service.cpp'
        serivce_file = open(serivcePath, 'w')

        mycode = []

        template_file = open('./Template/ServiceTemplate.cpp.tmpl', 'r')
        tmpl = jinja2Template(template_file.read())

        mycode.append(tmpl.render(
            NameSpace = NameSpace, ServiceName = ServiceName, rpcArray = rpcArray))

        serivce_file.writelines(mycode)
        serivce_file.close()

        print('Generate Service Done:'+ServiceName)


    def GenerateServiceStubHeader(self,NameSpace,ServiceName,rpcArray):
        serivceStubHeaderPath = './'+ServiceName+'ServiceStub.hpp'
        serivceStubHeader_file = open(serivceStubHeaderPath, 'w')

        mycode = []

        template_file = open('./Template/ServiceStubTemplate.hpp.tmpl', 'r')
        tmpl = jinja2Template(template_file.read())

        mycode.append(tmpl.render(
            NameSpace = NameSpace, ServiceName = ServiceName, rpcArray = rpcArray))

        serivceStubHeader_file.writelines(mycode)
        serivceStubHeader_file.close()

        print('Generate Service Stub Header Done:'+ServiceName)

    def GenerateServiceStub(self,NameSpace,ServiceName,rpcArray):
        serivceStubPath = './'+ServiceName+'ServiceStub.cpp'
        serivceStub_file = open(serivceStubPath, 'w')

        mycode = []

        template_file = open('./Template/ServiceStubTemplate.cpp.tmpl', 'r')
        tmpl = jinja2Template(template_file.read())

        mycode.append(tmpl.render(
            NameSpace = NameSpace, ServiceName = ServiceName, rpcArray = rpcArray))

        serivceStub_file.writelines(mycode)
        serivceStub_file.close()

        print('Generate Service Stub Done:'+ServiceName)


    def GenerateServiceProviderHeader(self,AppName, NameSpace,ServiceNameArray,ServiceArray):
        serivceProviderHeaderPath = './ServiceProvider_'+AppName+'.hpp'
        serivceProviderHeader_file = open(serivceProviderHeaderPath, 'w')

        mycode = []

        template_file = open('./Template/ServiceProviderTemplate.hpp.tmpl', 'r')
        tmpl = jinja2Template(template_file.read())

        mycode.append(tmpl.render(
            AppName = AppName, NameSpace = NameSpace, ServiceNameArray = ServiceNameArray, ServiceArray = ServiceArray))

        serivceProviderHeader_file.writelines(mycode)
        serivceProviderHeader_file.close()

        print('Generate Service Provider Header Done: ServiceProvider_'+AppName)

    def GenerateServiceProvider(self,AppName, NameSpace,ServiceNameArray,ServiceArray):
        serivceProviderPath = './ServiceProvider_'+AppName+'.cpp'
        serivceProvider_file = open(serivceProviderPath, 'w')

        mycode = []

        template_file = open('./Template/ServiceProviderTemplate.cpp.tmpl', 'r')
        tmpl = jinja2Template(template_file.read())

        mycode.append(tmpl.render(
            AppName = AppName, NameSpace = NameSpace, ServiceNameArray = ServiceNameArray, ServiceArray = ServiceArray))

        serivceProvider_file.writelines(mycode)
        serivceProvider_file.close()

        print('Generate Service Provider Header Done: ServiceProvider_'+AppName)


    def GenerateServiceUserHeader(self,AppName, NameSpace,ServiceNameArray,ServiceArray):
        serivceUserHeaderPath = './ServiceUser_'+AppName+'.hpp'
        serivceUserHeader_file = open(serivceUserHeaderPath, 'w')

        mycode = []

        template_file = open('./Template/ServiceUserTemplate.hpp.tmpl', 'r')
        tmpl = jinja2Template(template_file.read())

        mycode.append(tmpl.render(
            AppName = AppName, NameSpace = NameSpace, ServiceNameArray = ServiceNameArray, ServiceArray = ServiceArray))

        serivceUserHeader_file.writelines(mycode)
        serivceUserHeader_file.close()

        print('Generate Service User Header Done: ServiceUser_'+AppName)

    def GenerateServiceUser(self,AppName, NameSpace,ServiceNameArray,ServiceArray):
        serivceUserPath = './ServiceUser_'+AppName+'.cpp'
        serivceUser_file = open(serivceUserPath, 'w')

        mycode = []

        template_file = open('./Template/ServiceUserTemplate.cpp.tmpl', 'r')
        tmpl = jinja2Template(template_file.read())

        mycode.append(tmpl.render(
            AppName = AppName, NameSpace = NameSpace, ServiceNameArray = ServiceNameArray, ServiceArray = ServiceArray))

        serivceUser_file.writelines(mycode)
        serivceUser_file.close()

        print('Generate Service User Header Done: ServiceUser_'+AppName)

    def GenerateServiceAndStubs(self,NameSpace,ServiceName,rpcNameArray):
        self.GenerateServiceHeader(NameSpace,ServiceName,rpcNameArray)
        self.GenerateService(NameSpace,ServiceName,rpcNameArray)
        self.GenerateServiceStubHeader(NameSpace,ServiceName,rpcNameArray)
        self.GenerateServiceStub(NameSpace,ServiceName,rpcNameArray)

    def GenerateServiceForProvider(self,AppName,NameSpace,ServiceNameArray,ServiceArray):
        self.GenerateServiceProviderHeader(AppName,NameSpace,ServiceNameArray,ServiceArray)
        self.GenerateServiceProvider(AppName,NameSpace,ServiceNameArray,ServiceArray)
    
    def GenerateServiceForUser(self,AppName,NameSpace,ServiceNameArray,ServiceArray):
        self.GenerateServiceUserHeader(AppName,NameSpace,ServiceNameArray,ServiceArray)
        self.GenerateServiceUser(AppName,NameSpace,ServiceNameArray,ServiceArray)

if __name__ == '__main__':
    build = NDNSFCodeGenerator()
    build.GenerateServiceAndStubs("muas","ObjectDetection",[["YOLOv8","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
        ["YOLOv8_S","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"]])
    build.GenerateServiceAndStubs("muas","ManualControl",[["Takeoff","muas::ManualControl_Takeoff_Request","muas::ManualControl_Takeoff_Response"],
        ["Land","muas::ManualControl_Land_Request","muas::ManualControl_Land_Response"]])
    build.GenerateServiceForUser("GS","muas",["ObjectDetection","ManualControl"],[["ObjectDetection","YOLOv8","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
        ["ObjectDetection","YOLOv8_S","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
        ["ManualControl","Takeoff","muas::ManualControl_Takeoff_Request","muas::ManualControl_Takeoff_Response"],
        ["ManualControl","Land","muas::ManualControl_Land_Request","muas::ManualControl_Land_Response"]])
    build.GenerateServiceForUser("Drone","muas",["ObjectDetection"],[["ObjectDetection","YOLOv8","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
        ["ObjectDetection","YOLOv8_S","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"]])
    build.GenerateServiceForProvider("GS","muas",["ObjectDetection"],[["ObjectDetection","YOLOv8","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
        ["ObjectDetection","YOLOv8_S","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"]])
    build.GenerateServiceForProvider("Drone","muas",["ObjectDetection","ManualControl"],[["ObjectDetection","YOLOv8","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
        ["ObjectDetection","YOLOv8_S","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
        ["ManualControl","Takeoff","muas::ManualControl_Takeoff_Request","muas::ManualControl_Takeoff_Response"],
        ["ManualControl","Land","muas::ManualControl_Land_Request","muas::ManualControl_Land_Response"]])
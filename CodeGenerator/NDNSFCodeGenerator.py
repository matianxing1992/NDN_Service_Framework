#! /usr/bin/env python
# coding=utf-8
# https://blog.csdn.net/qq_34199383/article/details/81381557

from string import Template

from jinja2 import Template as jinja2Template

import yaml

import os

def writeToFile(filePath,content):
    file = open(filePath, 'w')

    mycode = []
    mycode.append(content)

    file.writelines(mycode)
    file.close()

class NDNSFCodeGenerator:

    # rpcNameArray =[[rpcName,rpcRequestMessage,rpcResponseMessage]]
    def GenerateServiceHeader(self,NameSpace,ServiceName,rpcArray):
        serivcePath = './Generated/'+ServiceName+'Service.hpp'
        serivce_file = open(serivcePath, 'w')

        mycode = []

        template_file = open('./Template/ServiceTemplate.hpp.tmpl', 'r')
        tmpl = jinja2Template(template_file.read())

        mycode.append(tmpl.render(
            NameSpace = NameSpace, ServiceName = ServiceName, rpcArray = rpcArray))

        serivce_file.writelines(mycode)
        serivce_file.close()

        print('Generate Service Header Done:'+ServiceName)


    def GenerateService(self,NameSpace,ServiceName,rpcArray):
        serivcePath = './Generated/'+ServiceName+'Service.cpp'
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
        serivceStubHeaderPath = './Generated/'+ServiceName+'ServiceStub.hpp'
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
        serivceStubPath = './Generated/'+ServiceName+'ServiceStub.cpp'
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
        serivceProviderHeaderPath = './Generated/ServiceProvider_'+AppName+'.hpp'
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
        serivceProviderPath = './Generated/ServiceProvider_'+AppName+'.cpp'
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
        serivceUserHeaderPath = './Generated/ServiceUser_'+AppName+'.hpp'
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
        serivceUserPath = './Generated/ServiceUser_'+AppName+'.cpp'
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

    def GenerateServiceForProvider(self,ServiceProviderName,NameSpace,ServiceNameArray,ServiceArray):
        self.GenerateServiceProviderHeader(ServiceProviderName,NameSpace,ServiceNameArray,ServiceArray)
        self.GenerateServiceProvider(ServiceProviderName,NameSpace,ServiceNameArray,ServiceArray)
    
    def GenerateServiceForUser(self,ServiceUserName,NameSpace,ServiceNameArray,ServiceArray):
        self.GenerateServiceUserHeader(ServiceUserName,NameSpace,ServiceNameArray,ServiceArray)
        self.GenerateServiceUser(ServiceUserName,NameSpace,ServiceNameArray,ServiceArray)

if __name__ == '__main__':

    build = NDNSFCodeGenerator()
    with open("app.yml", "r") as stream:
        try:
            config = yaml.safe_load(stream)
            # print(config)
            NameSpace = config['NameSpace']
            # print(NameSpace)
            MessageProto = config['MessageProto']
            # print(MessageProto)
            writeToFile("./Generated/messages.proto",MessageProto)
            os.system("protoc --proto_path=./Generated --cpp_out=./Generated messages.proto")

            ServiceNameArray = [service['serviceName'] for service in config['Services']]
            # print(ServiceNameArray)

            ServiceDict = {}

            # print(config['Services'])
            for service in config['Services']:
                # print(service['serviceName'])
                # print(service['serviceDescription'])
                rpcNameArray = [[rpc['name'], rpc['requestMessage'], rpc['responseMessage']] for rpc in service['RPCList']]
                # print(rpcNameArray)
                build.GenerateServiceAndStubs(NameSpace,service['serviceName'],rpcNameArray)
                rpcNameArray = [[service['serviceName'], rpc['name'], rpc['requestMessage'], rpc['responseMessage']] for rpc in service['RPCList']]
                ServiceDict[service['serviceName']] = rpcNameArray

            # print(ServiceDict)

            # print(config['ServiceProviders'])
            for ServiceProvider in config['ServiceProviders']:
                # print(ServiceProvider['providerName'])
                # print(ServiceProvider['providerDescription'])
                # print(ServiceProvider['ServiceList'])
                ServiceArray = []
                for ServiceName in ServiceProvider['ServiceList']:
                    for d in ServiceDict[ServiceName]:
                        # d.insert(0, ServiceName)
                        ServiceArray.append(d)
                # print(ServiceArray)
                build.GenerateServiceForProvider(ServiceProvider['providerName'],NameSpace,ServiceProvider['ServiceList'],ServiceArray)
            
            # print(config['ServiceUsers'])
            for ServiceUser in config['ServiceUsers']:
                # print(ServiceUser['userName'])
                # print(ServiceUser['userDescription'])
                # print(ServiceUser['ServiceStubList'])
                ServiceStubArray = []
                for ServiceName in ServiceUser['ServiceStubList']:
                    for d in ServiceDict[ServiceName]:
                        ServiceStubArray.append(d)
                # print(ServiceStubArray)
                build.GenerateServiceForUser(ServiceUser['userName'],NameSpace,ServiceUser['ServiceStubList'],ServiceStubArray)
            
        except yaml.YAMLError as exc:
            print(exc)

    # build.GenerateServiceAndStubs("muas","ObjectDetection",[["YOLOv8","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
    #     ["YOLOv8_S","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"]])
    # build.GenerateServiceAndStubs("muas","ManualControl",[["Takeoff","muas::ManualControl_Takeoff_Request","muas::ManualControl_Takeoff_Response"],
    #     ["Land","muas::ManualControl_Land_Request","muas::ManualControl_Land_Response"]])
    # build.GenerateServiceForUser("GS","muas",["ObjectDetection","ManualControl"],[["ObjectDetection","YOLOv8","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
    #     ["ObjectDetection","YOLOv8_S","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
    #     ["ManualControl","Takeoff","muas::ManualControl_Takeoff_Request","muas::ManualControl_Takeoff_Response"],
    #     ["ManualControl","Land","muas::ManualControl_Land_Request","muas::ManualControl_Land_Response"]])
    # build.GenerateServiceForUser("Drone","muas",["ObjectDetection"],[["ObjectDetection","YOLOv8","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
    #     ["ObjectDetection","YOLOv8_S","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"]])
    # build.GenerateServiceForProvider("GS","muas",["ObjectDetection"],[["ObjectDetection","YOLOv8","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
    #     ["ObjectDetection","YOLOv8_S","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"]])
    # build.GenerateServiceForProvider("Drone","muas",["ObjectDetection","ManualControl"],[["ObjectDetection","YOLOv8","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
    #     ["ObjectDetection","YOLOv8_S","muas::ObjectDetection_YOLOv8_Request","muas::ObjectDetection_YOLOv8_Response"],
    #     ["ManualControl","Takeoff","muas::ManualControl_Takeoff_Request","muas::ManualControl_Takeoff_Response"],
    #     ["ManualControl","Land","muas::ManualControl_Land_Request","muas::ManualControl_Land_Response"]])
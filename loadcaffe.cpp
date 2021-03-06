//
//  main.cpp
//  loadcaffe
//
//  Created by Sergey Zagoruyko on 28/11/14.
//  Copyright (c) 2014 Sergey Zagoruyko. All rights reserved.
//

#include <iostream>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>

#include <TH/TH.h>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>

#include "build/caffe.pb.h"

using google::protobuf::io::FileInputStream;
using google::protobuf::io::FileOutputStream;
using google::protobuf::io::ZeroCopyInputStream;
using google::protobuf::io::CodedInputStream;
using google::protobuf::Message;


extern "C" {
void loadBinary(void** handle, const char* prototxt_name, const char* binary_name);
void convertProtoToLua(void** handle, const char* lua_name, const char* cuda_package);
void loadModule(const void** handle, const char* name, THFloatTensor* weight, THFloatTensor* bias);
void destroyBinary(void** handle);
}


bool ReadProtoFromTextFile(const char* filename, Message* proto) {
    int fd = open(filename, O_RDONLY);
    if(fd < 0)
      return false;
    
    FileInputStream* input = new FileInputStream(fd);
    bool success = google::protobuf::TextFormat::Parse(input, proto);
    delete input;
    close(fd);
    return success;
}


bool ReadProtoFromBinaryFile(const char* filename, Message* proto) {
    int fd = open(filename, O_RDONLY);
    if(fd < 0)
      return false;
    
    ZeroCopyInputStream* raw_input = new FileInputStream(fd);
    CodedInputStream* coded_input = new CodedInputStream(raw_input);
    coded_input->SetTotalBytesLimit(1073741824, 536870912);
    
    bool success = proto->ParseFromCodedStream(coded_input);
    
    delete coded_input;
    delete raw_input;
    close(fd);
    return success;
}


enum PACKAGE_TYPE {
  CCN2, NN, CUDNN
};


void convertProtoToLua(void** handle, const char* lua_name, const char* cuda_package)
{
  const caffe::NetParameter netparam = *(const caffe::NetParameter*)handle[1];

  PACKAGE_TYPE cuda_package_type = CCN2;
  if(std::string(cuda_package) == "ccn2")
    cuda_package_type = CCN2;
  else if(std::string(cuda_package) == "nn")
    cuda_package_type = NN;
  else if(std::string(cuda_package) == "cudnn")
    cuda_package_type = CUDNN;

  std::ofstream ofs (lua_name);

  ofs << "require '" << cuda_package << "'\n";
  ofs << "require 'cunn'\n";
  ofs << "model = {}\n";
  if(std::string(cuda_package)=="ccn2")
    ofs<< "table.insert(model, {'torch_transpose_dwhb', nn.Transpose({1,4},{1,3},{1,2})})\n";
  else if(std::string(cuda_package)=="nn" || std::string(cuda_package)=="cudnn")
    ofs<< "require 'inn'\n";
  
  int num_output = netparam.input_dim_size();
  for (int i=0; i<netparam.layers_size(); ++i)
  {
    std::vector<std::pair<std::string, std::string>> lines;
    auto& layer = netparam.layers(i);
    switch(layer.type())
    {
      case caffe::LayerParameter::CONVOLUTION:
      {
	auto &param = layer.convolution_param();
	int groups = param.group() == 0 ? 1 : param.group();
	int nInputPlane = layer.blobs(0).channels()*groups;
        int nOutputPlane = layer.blobs(0).num();
	//int nOutputPlane = param.num_output();
	num_output = nOutputPlane;
	int kW = param.kernel_w();
	int kH = param.kernel_h();
	int dW = param.stride_w();
	int dH = param.stride_h();
	if(kW==0 || kH==0)
	{
	  kW = param.kernel_size();
	  kH = kW;
	}
	if(dW==0 || dH==0)
	{
	  dW = param.stride();
	  dH = dW;
	}
	int pad_w = param.pad_w();
	int pad_h = param.pad_h();
        if(pad_w==0 || pad_h==0)
        {
          pad_w = param.pad();
          pad_h = pad_w;
        }
	if(cuda_package_type == CCN2)
	{
          if(kW != kH || dW != dH || pad_w != pad_h)
          {
            std::cout << "ccn2 only supports square images!\n";
            break;
          }
	  char buf[1024];
	  sprintf(buf, "ccn2.SpatialConvolution(%d, %d, %d, %d, %d, %d)", 
	      nInputPlane, nOutputPlane, kW, dW, pad_w, groups);
	  lines.emplace_back(layer.name(), buf);
	}
	else
	{
	  char buf[1024];
	  const char* mm_or_not = std::string(cuda_package)=="nn" ? "MM" : "";
	  sprintf(buf, "%s.SpatialConvolution%s(%d, %d, %d, %d, %d, %d, %d, %d, %d)", 
	      cuda_package, mm_or_not, nInputPlane, nOutputPlane, kW, kH, dW, dH, pad_w, pad_h, groups);
	  lines.emplace_back(layer.name(), buf);
	}
	break;
      }
      case caffe::LayerParameter::POOLING:
      {
	auto &param = layer.pooling_param();
	std::string ptype = param.pool() == caffe::PoolingParameter::MAX ? "Max" : "Avg";
	int kW = param.kernel_w();
	int kH = param.kernel_h();
	int dW = param.stride_w();
	int dH = param.stride_h();
	if(kW==0 || kH==0)
	{
	  kW = param.kernel_size();
	  kH = kW;
	}
	if(dW==0 || dH==0)
	{
	  dW = param.stride();
	  dH = dW;
	}

	char buf[1024];
	switch(cuda_package_type)
	{
	  case CCN2:
	    sprintf(buf, "ccn2.Spatial%sPooling(%d, %d)", ptype.c_str(), kW, dW);
	    break;
	  case CUDNN:
	    sprintf(buf, "%s.Spatial%sPooling(%d, %d, %d, %d):ceil()", cuda_package, ptype=="Avg" ? "Average" : "Max", kW, kH, dW, dH);
	    break;
	  case NN:
	    sprintf(buf, "inn.Spatial%sPooling(%d, %d, %d, %d)", ptype=="Avg" ? "Average" : "Max", kW, kH, dW, dH);
	    break;
	}
	lines.emplace_back(layer.name(), buf);
	break;
      }
      case caffe::LayerParameter::RELU:
      {
	switch(cuda_package_type)
	{
	  case CUDNN:
	    lines.emplace_back(layer.name(), "cudnn.ReLU(true)");
	    break;
	  default:
	    lines.emplace_back(layer.name(), "nn.ReLU()");
	    break;
	}
	break;
      }
      case caffe::LayerParameter::LRN:
      {
        auto &param = layer.lrn_param();
        int local_size = param.local_size();
        float alpha = param.alpha();
        float beta = param.beta();
        float k = param.k();
        char buf[1024];
	if(std::string(cuda_package) == "ccn2")
	  sprintf(buf, "ccn2.SpatialCrossResponseNormalization(%d, %.6f, %.4f, %f)", local_size, alpha, beta, k);
        else
	  sprintf(buf, "inn.SpatialCrossResponseNormalization(%d, %.6f, %.4f, %f)", local_size, alpha, beta, k);
        lines.emplace_back(layer.name(), buf);
	break;
      }
      case caffe::LayerParameter::INNER_PRODUCT:
      {
	auto &param = layer.inner_product_param();
	int nInputPlane = layer.blobs(0).width();
	int nOutputPlane = param.num_output();
	char buf[1024];
	sprintf(buf, "nn.Linear(%d, %d)", nInputPlane, nOutputPlane);
	if(num_output != nInputPlane)
	{
	  if(std::string(cuda_package) == "ccn2")
	    lines.emplace_back("torch_transpose_bdwh", "nn.Transpose({4,1},{4,2},{4,3})");
	  lines.emplace_back("torch_view", "nn.View(-1):setNumInputDims(3)");
	}
	lines.emplace_back(layer.name(), buf);
	num_output = nOutputPlane;
	break;
      }
      case caffe::LayerParameter::DROPOUT:
      {
	char buf[1024];
	sprintf(buf, "nn.Dropout(%f)", netparam.layers(i).dropout_param().dropout_ratio());
	lines.emplace_back(layer.name(), buf);
	break;
      }
      case caffe::LayerParameter::SOFTMAX_LOSS:
      {
	lines.emplace_back(layer.name(), "nn.SoftMax()");
	break;
      }
      case caffe::LayerParameter::SOFTMAX:
      {
	lines.emplace_back(layer.name(), "nn.SoftMax()");
	break;
      }
      default:
      {
	std::cout << "MODULE " << netparam.layers(i).name() << " UNDEFINED\n";
	break;
      }
    }

    if(!lines.empty())
      for(auto& it: lines)
	ofs << "table.insert(model, {'" << it.first << "', " << it.second << "})\n";
    else
    {
      ofs << "-- module '" << layer.name() << "' not found\n";
      std::cout << "module '" << layer.name() << "' not found\n";
    }
  }
}


void loadBinary(void** handle, const char* prototxt_name, const char* binary_name)
{
  caffe::NetParameter* netparam = new caffe::NetParameter();
  ReadProtoFromTextFile(prototxt_name, netparam);
  bool success = ReadProtoFromBinaryFile(binary_name, netparam);
  if(success)
  {
    std::cout << "Successfully loaded " << binary_name << std::endl;
    handle[1] = netparam;
  }
  else
    std::cout << "Couldn't load " << binary_name << std::endl;
}

void destroyBinary(void** handle)
{
  const caffe::NetParameter* netparam2 = (const caffe::NetParameter*)handle[1];
  delete netparam2;
}

void loadModule(const void** handle, const char* name, THFloatTensor* weight, THFloatTensor* bias)
{
  if(handle == NULL)
  {
    std::cout << "network not loaded!\n";
    return;
  }

  const caffe::NetParameter* netparam = (const caffe::NetParameter*)handle[1];

  int n = netparam->layers_size();
  for(int i=0; i<n; ++i)
  {
    auto &layer = netparam->layers(i);
    if(std::string(name) == layer.name())
    {
      int nInputPlane = layer.blobs(0).channels();
      int nOutputPlane = layer.blobs(0).num();
      int kW = layer.blobs(0).width();
      int kH = layer.blobs(0).height();
      printf("%s: %d %d %d %d\n", name, nOutputPlane, nInputPlane, kW, kH);
      
      THFloatTensor_resize4d(weight, nOutputPlane, nInputPlane, kW, kH);
      memcpy(THFloatTensor_data(weight), layer.blobs(0).data().data(), sizeof(float)*nOutputPlane*nInputPlane*kW*kH);

      THFloatTensor_resize1d(bias, layer.blobs(1).data_size());
      memcpy(THFloatTensor_data(bias), layer.blobs(1).data().data(), sizeof(float)*layer.blobs(1).data_size());
    }
  }
}

#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <set>
#include <cstdio>
#include <stdexcept>

static auto read(const std::string& path) -> std::vector<char> {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if (!f || f.tellg()<0) throw std::runtime_error("cannot open "+path);
    std::vector<char> bytes(static_cast<size_t>(f.tellg())); f.seekg(0);
    if (!f.read(bytes.data(),bytes.size())) throw std::runtime_error("cannot read "+path);
    return bytes;
}
static void append(std::vector<char>& out, std::string_view name, const std::vector<char>& bytes, unsigned mode, unsigned ino) {
    if (bytes.size()>UINT32_MAX || name.size()>=UINT32_MAX) throw std::runtime_error("archive entry too large");
    char h[111];
    std::snprintf(h,sizeof(h),"070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",
        ino,mode,0,0,1,0,static_cast<unsigned>(bytes.size()),0,0,0,0,static_cast<unsigned>(name.size()+1),0);
    out.insert(out.end(),h,h+110); out.insert(out.end(),name.begin(),name.end()); out.push_back(0);
    out.resize((out.size()+3)&~size_t{3}); out.insert(out.end(),bytes.begin(),bytes.end()); out.resize((out.size()+3)&~size_t{3});
}
int main(int argc,char** argv) {
    try {
        if (argc<3) throw std::runtime_error("usage: initramfs OUTPUT NAME=FILE...");
        std::vector<char> out; std::set<std::string> names;
        for (int i=2;i<argc;++i) {
            std::string spec=argv[i];
            auto split=spec.find('='); if(split==std::string::npos) throw std::runtime_error("expected NAME=FILE");
            auto name=spec.substr(0,split); if(name.empty() || !names.insert(name).second) throw std::runtime_error("duplicate or empty name");
            auto bytes=read(spec.substr(split+1));
            append(out,name,bytes,0100444,names.size());
        }
        append(out,"TRAILER!!!",{},0,0);
        std::ofstream f(argv[1],std::ios::binary|std::ios::trunc); f.write(out.data(),out.size());
        if(!f) throw std::runtime_error("cannot write archive");
        std::cout<<"initramfs: "<<names.size()<<" files, "<<out.size()<<" bytes\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

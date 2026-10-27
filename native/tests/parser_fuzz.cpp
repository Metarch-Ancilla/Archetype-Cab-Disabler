// Deterministic mutation stress for the production parser; pair with ASan/UBSan.
#include "../backend/core.hpp"
#include <random>
#include <iostream>
using namespace cab;
int main() {
    std::mt19937 random(0x43414232);
    std::vector<Bytes> seeds={"<p cabActive=\"true\"/>","<p><cabParameters sectionActive=\"0\"/></p>",
        "<!DOCTYPE p [<!ENTITY x 'x'>]><p cabActive=\"1\">&x;</p>",
        Bytes("p\0\x01\x01" "cabActive\0\x01\x01\x02\0",18),
        encode16("<p cabActive=\"1\"/>",false),encode16("<p cabActive=\"0\"/>",true)};
    for(size_t i=0;i<10000;++i) {
        Bytes data=seeds[random()%seeds.size()];
        for(unsigned mutations=random()%12;mutations;--mutations) {
            size_t at=random()%(data.size()+1);
            switch(random()%4) {
                case 0: if(at<data.size())data[at]=(char)random();break;
                case 1: data.insert(at,1,(char)random());break;
                case 2: if(at<data.size())data.erase(at,1+random()%(data.size()-at));break;
                default: data.resize(at);break;
            }
        }
        std::optional<Inspection> recognized;
        try { recognized=inspect(data); } catch(const Error &) { continue; }
        try {
            auto info=*recognized;
            for(bool enabled:{false,true}) {
                auto output=patch(data,enabled);auto after=inspect(output);
                require(after.enabled==enabled&&data.substr(0,info.start)==output.substr(0,after.start)&&
                    data.substr(info.end)==output.substr(after.end),"Mutation changed bytes outside scalar");
            }
        } catch(const Error &error) { std::cerr<<error.what()<<'\n';return 1; }
    }
    for(const Bytes &data:{"<p cabActive=\"1\" "+Bytes(65536,'a')+"=\"v\"/>",
                           "<p cabActive=\"1\">"+Bytes(1024*1024,'&')+"</p>"}) {
        try{inspect(data);}catch(const Error &){}
    }
    std::cout<<"10000 deterministic parser mutations and bounded large malformed cases passed\n";
}

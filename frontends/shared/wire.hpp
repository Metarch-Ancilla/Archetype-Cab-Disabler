#pragma once
// Protocol-only code shared by native views. No preset parsing or file writes.
#include "../../shared/text.hpp"
#include "../../shared/preset_copy.hpp"
#include <string>
#include <vector>
#include <stdexcept>
#include <utility>

namespace nd {
struct Option {
    std::string value, label; bool selected=false;
    int parent=-1; std::string kind; bool enabled=true; std::string location;
};
struct Control {
    std::string id, kind, label, value; int height=30;
    std::vector<Option> options;
    const Option &selectedOption() const {
        for (const auto &option : options) if (option.value == value) return option;
        throw std::runtime_error("Selected option missing from control");
    }
};
inline std::string editDescription(const Control &cab,const Control &mode) {
    return cab.selectedOption().location+"\n"+
           (mode.value=="1" ? copyDescription(cab.value=="0") : mode.selectedOption().location);
}
struct Model {
    int page=0; bool busy=false, back=false, next=false;
    std::string nextLabel, title, status, completion;
    long edited=0,total=0;
    size_t snapshotBytes=0, optionCount=0;
    std::vector<Control> controls;
    // Returns true only when an entire snapshot is available for rendering.
    bool record(const std::vector<std::string> &p) {
        if (p.at(0)=="STATE") snapshotBytes=optionCount=0;
        if (p[0]=="STATE"||p[0]=="CONTROL"||p[0]=="OPTION") {
            for (const auto &field:p) {
                if (field.size()>128*1024*1024-snapshotBytes)throw std::runtime_error("Backend snapshot exceeds safety limit");
                snapshotBytes+=field.size();
            }
        }
        if(p.at(0)=="HELLO") {
            if(p.at(1)!="2")throw std::runtime_error("Unsupported backend protocol");
        } else if(p[0]=="STATE") {
            controls.clear(); page=std::stoi(p.at(1)); busy=p.at(2)=="1";
            back=p.at(3)=="1"; next=p.at(4)=="1"; nextLabel=unhex(p.at(5));
            title=unhex(p.at(6)); status=unhex(p.at(7)); completion=unhex(p.at(8));
            edited=std::stol(p.at(9)); total=std::stol(p.at(10));
        } else if(p[0]=="CONTROL") {
            if(controls.size()>=16)throw std::runtime_error("Too many backend controls");
            Control c; c.id=p.at(1); c.kind=p.at(2); c.height=std::stoi(p.at(3));
            c.label=unhex(p.at(4)); c.value=unhex(p.at(5)); controls.push_back(c);
        } else if(p[0]=="OPTION") {
            if(controls.empty() || controls.back().id!=p.at(1))throw std::runtime_error("Invalid backend option");
            if(++optionCount>100001)throw std::runtime_error("Too many backend options");
            Option option;option.value=unhex(p.at(2));option.label=unhex(p.at(3));option.selected=p.at(4)=="1";
            option.parent=std::stoi(p.at(5));option.kind=p.at(6);option.enabled=p.at(7)=="1";option.location=unhex(p.at(8));
            if(option.parent >= (int)controls.back().options.size() || option.parent < -1)
                throw std::runtime_error("Invalid checkbox tree parent");
            controls.back().options.push_back(std::move(option));
        } else if(p[0]=="END") return true;
        else if(p[0]=="PROGRESS") { edited=std::stol(p.at(1)); total=std::stol(p.at(2)); }
        else if(p[0]=="STATUS") status=unhex(p.at(1));
        return false;
    }
    bool indexing() const { return page==0 && busy; }
    std::string progress() const {
        return (busy ? "Working..." : completion)+" "+
               std::to_string(edited)+"/"+std::to_string(total);
    }
};
// Frontends can paint the first page before the backend process is ready.
inline Model indexingModel() {
    Model model;model.busy=true;model.title="Select Plugins";model.nextLabel="Next";
    Control plugins;plugins.id="plugins";plugins.kind="plugins";plugins.height=400;
    Control browse;browse.id="browse";browse.kind="buttons";browse.label="Preset folders not detected?";
    Option add;add.value="add";add.label="Browse…";browse.options.push_back(std::move(add));
    model.controls.push_back(std::move(plugins));model.controls.push_back(std::move(browse));
    return model;
}
inline std::vector<std::vector<size_t>> treeChildren(const Control &control) {
    std::vector<std::vector<size_t>> children(control.options.size()+1);
    for(size_t i=0;i<control.options.size();++i) {
        int parent=control.options[i].parent;
        children[parent<0?control.options.size():(size_t)parent].push_back(i);
    }return children;
}
inline std::vector<int> treeStates(const Control &control) {
    size_t size=control.options.size();std::vector<size_t> all(size),selected(size);std::vector<int> states(size);
    for(size_t i=size;i-->0;) {
        const auto &o=control.options[i];
        if(o.kind=="preset"&&o.enabled){all[i]=1;selected[i]=o.selected?1:0;}
        states[i]=selected[i]==0?0:selected[i]==all[i]?2:1;
        if(o.parent>=0){all[(size_t)o.parent]+=all[i];selected[(size_t)o.parent]+=selected[i];}
    }return states;
}
inline void toggleTree(Control &control,size_t index,bool selected) {
    if(index>=control.options.size()||!control.options[index].enabled)return;
    std::vector<bool> affected(control.options.size());
    for(size_t i=0;i<control.options.size();++i) {
        auto &o=control.options[i];affected[i]=i==index||(o.parent>=0&&affected[(size_t)o.parent]);
        if(affected[i]&&o.kind=="preset"&&o.enabled)o.selected=selected;
    }
}
// Keep the displayed model intact across partial pipe reads. Field collection
// and native callbacks must never observe a half-received page.
inline bool receive(Model &current, Model &pending, bool &reading,
                    const std::vector<std::string> &record) {
    if(record.at(0)=="STATE") {
        if(reading)throw std::runtime_error("Incomplete backend snapshot");
        pending=Model{};reading=true;
    }
    if(reading) {
        if(pending.record(record)) {
            current=std::move(pending);reading=false;return true;
        }
    } else {
        if(record[0]=="CONTROL"||record[0]=="OPTION"||record[0]=="END")
            throw std::runtime_error("Backend record outside snapshot");
        current.record(record);
    }
    return false;
}
using Fields=std::vector<std::pair<std::string,std::string>>;
inline std::string command(const std::string &action,const Fields &fields={}) {
    std::string out=action;
    for(const auto &f:fields)out+='\t'+f.first+'='+hex(f.second);
    return out+'\n';
}
} // namespace nd

#pragma once

class context {
protected:
    virtual void finish(int r) = 0;
public:
    context() {}
    virtual ~context() {}       
    virtual void complete(int r) {
        finish(r);
        delete this;
    }  
};
#include <gui/server.hpp>

int main() {
    ubus_conn parent;
    auto status = parent.bind(_startup().bootstrap); if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = parent.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    return gui_serve(parent, std::move(setup));
}

#include "sorx/appliance/app_main.hpp"

int main(const int argc, const char* const argv[]) {
    return sorx::appliance::run_appliance_app(
        sorx::appliance::AppKind::DataPlane, argc, argv);
}

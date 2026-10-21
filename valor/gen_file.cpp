// Crea el archivo del conjunto de trabajo con el mismo contenido que tests/common.hpp (semilla 42).
#include <cstdlib>
#include "../tests/common.hpp"
int main(int argc, char** argv) { tt::make_file(argv[1], std::strtoull(argv[2], nullptr, 10) << 20, 42); return 0; }

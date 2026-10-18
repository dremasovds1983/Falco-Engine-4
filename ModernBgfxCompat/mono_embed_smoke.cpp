#include <mono/jit/jit.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/class.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/image.h>
#include <mono/metadata/mono-config.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace
{
    int fail(const std::string& message, int code)
    {
        std::cerr << message << std::endl;
        return code;
    }
}

int main(int argc, char** argv)
{
    if (argc != 4)
        return fail("Usage: MonoEmbedSmoke <Mono root> <FalcoEngine.dll> <default|v4>", 2);

    const std::filesystem::path monoRoot = std::filesystem::absolute(argv[1]);
    const std::filesystem::path assemblyPath = std::filesystem::absolute(argv[2]);
    const std::string runtimeMode = argv[3];
    const std::string monoLib = (monoRoot / "lib").string();
    const std::string monoEtc = (monoRoot / "etc").string();
    const std::string assembly = assemblyPath.string();

    std::cout << "Mono lib: " << monoLib << std::endl;
    std::cout << "Mono etc: " << monoEtc << std::endl;
    std::cout << "Assembly: " << assembly << std::endl;
    std::cout << "Runtime mode: " << runtimeMode << std::endl;

    if (!std::filesystem::is_directory(monoLib) || !std::filesystem::is_directory(monoEtc))
        return fail("Mono lib/etc directory is missing.", 3);
    if (!std::filesystem::is_regular_file(assemblyPath))
        return fail("Managed assembly is missing.", 4);

    mono_set_dirs(monoLib.c_str(), monoEtc.c_str());
    mono_config_parse(nullptr);

    MonoDomain* rootDomain = nullptr;
    if (runtimeMode == "default")
        rootDomain = mono_jit_init("Falco_Embed_Smoke_Root");
    else if (runtimeMode == "v4")
        rootDomain = mono_jit_init_version("Falco_Embed_Smoke_Root", "v4.0.30319");
    else
        return fail("Runtime mode must be default or v4.", 5);

    std::cout << "Root domain: " << rootDomain << std::endl;
    if (rootDomain == nullptr)
        return fail("mono_jit_init failed.", 6);

    const mono_bool rootSet = mono_domain_set(rootDomain, false);
    std::cout << "Root domain set: " << static_cast<int>(rootSet) << std::endl;
    std::cout << "Current root matches: " << (mono_domain_get() == rootDomain) << std::endl;

    MonoDomain* appDomain = mono_domain_create_appdomain(
        const_cast<char*>("Falco_Embed_Smoke_App"), nullptr);
    std::cout << "App domain: " << appDomain << std::endl;
    if (appDomain == nullptr)
        return fail("mono_domain_create_appdomain failed.", 7);

    const mono_bool appSet = mono_domain_set(appDomain, false);
    std::cout << "App domain set: " << static_cast<int>(appSet) << std::endl;
    std::cout << "Current app matches: " << (mono_domain_get() == appDomain) << std::endl;

    MonoImageOpenStatus imageStatus = MONO_IMAGE_IMAGE_INVALID;
    MonoImage* probeImage = mono_image_open(assembly.c_str(), &imageStatus);
    std::cout << "mono_image_open status: " << static_cast<int>(imageStatus)
              << " (" << mono_image_strerror(imageStatus) << ")" << std::endl;
    std::cout << "Probe image: " << probeImage << std::endl;
    if (probeImage != nullptr)
        mono_image_close(probeImage);

    MonoAssembly* loadedAssembly = mono_domain_assembly_open(appDomain, assembly.c_str());
    std::cout << "Domain assembly: " << loadedAssembly << std::endl;
    if (loadedAssembly == nullptr)
        return fail("mono_domain_assembly_open failed.", 8);

    MonoImage* loadedImage = mono_assembly_get_image(loadedAssembly);
    std::cout << "Assembly image: " << loadedImage << std::endl;
    if (loadedImage == nullptr)
        return fail("mono_assembly_get_image failed.", 9);

    MonoClass* requiredClass = mono_class_from_name(loadedImage, "FalcoEngine", "MonoBehaviour");
    std::cout << "FalcoEngine.MonoBehaviour: " << requiredClass << std::endl;
    if (requiredClass == nullptr)
        return fail("Required managed class is missing.", 10);

    std::cout << "Native Mono embedding smoke passed." << std::endl;
    return 0;
}

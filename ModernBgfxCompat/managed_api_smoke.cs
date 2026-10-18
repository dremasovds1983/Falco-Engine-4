using System;
using System.IO;
using System.Reflection;

internal static class ManagedApiSmoke
{
    private static readonly string[] RequiredTypes =
    {
        "FalcoEngine.MonoBehaviour",
        "FalcoEngine.Transform",
        "FalcoEngine.GameObject",
        "FalcoEngine.Vector2",
        "FalcoEngine.Vector3",
        "FalcoEngine.Vector4",
        "FalcoEngine.Quaternion",
        "FalcoEngine.Object"
    };

    private static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("Usage: ManagedApiSmoke <FalcoEngine.dll>");
            return 2;
        }

        string path = Path.GetFullPath(args[0]);
        Console.WriteLine("Managed API path: " + path);

        if (!File.Exists(path))
        {
            Console.Error.WriteLine("Managed API file does not exist.");
            return 3;
        }

        try
        {
            Assembly assembly = Assembly.LoadFrom(path);
            Console.WriteLine("Assembly: " + assembly.FullName);
            Console.WriteLine("Image runtime: " + assembly.ImageRuntimeVersion);

            AssemblyName[] references = assembly.GetReferencedAssemblies();
            Console.WriteLine("References: " + references.Length);
            foreach (AssemblyName reference in references)
                Console.WriteLine("  " + reference.FullName);

            Type[] types = assembly.GetTypes();
            Console.WriteLine("Types: " + types.Length);

            foreach (string requiredType in RequiredTypes)
            {
                bool found = assembly.GetType(requiredType, false) != null;
                Console.WriteLine("Required type " + requiredType + ": " + found);
                if (!found)
                    return 4;
            }

            Console.WriteLine("Managed API smoke passed.");
            return 0;
        }
        catch (ReflectionTypeLoadException exception)
        {
            Console.Error.WriteLine(exception.ToString());
            foreach (Exception loaderException in exception.LoaderExceptions)
                Console.Error.WriteLine("Loader exception: " + loaderException);
            return 5;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception.ToString());
            return 6;
        }
    }
}

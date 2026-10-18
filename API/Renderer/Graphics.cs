using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Text;

namespace FalcoEngine
{
    public static class Graphics
    {
        /// <summary>
        /// Queue an opaque PNG of the main window backbuffer (including UI).
        /// The file is written asynchronously; completion or failure is logged.
        /// In Editor this captures the main window, not an isolated camera.
        /// </summary>
        public static void CaptureScreenshot(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) throw new ArgumentException("Screenshot path is empty", "path");
            if (path.IndexOf('\0') >= 0) throw new ArgumentException("Invalid screenshot path", "path");
            string fullPath = System.IO.Path.GetFullPath(path);
            if (!string.Equals(System.IO.Path.GetExtension(fullPath), ".png", StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Screenshot path must end in .png", "path");
            System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(fullPath));
            if (!INTERNAL_captureScreenshot(fullPath))
                throw new NotSupportedException("Screenshot capture requires the modern bgfx renderer");
        }

        [MethodImpl(MethodImplOptions.InternalCall)]
        private static extern bool INTERNAL_captureScreenshot(string path);

        /// <summary>
        /// Render quad to the render target using material.
        /// In OnRenderImage, use an HDR target and a shader that preserves linear RGB.
        /// Do not apply tonemapping or sRGB encoding here: camera presentation does so
        /// after the post-effect chain. LDR targets clip values above one.
        /// Calls are queued in order (up to 16 per camera frame) and require an
        /// active OnRenderImage callback. Use distinct source/destination textures.
        /// Keep targets alive until the camera frame has completed; reuse them
        /// across frames rather than disposing them inside the callback.
        /// Each pass replaces the target color; it does not alpha-blend the result.
        /// </summary>
        /// <param name="material">Material</param>
        /// <param name="target">Render target</param>
        /// <param name="copyToBackBuffer">Render to screen or to render target only</param>
        public static void Blit(Material material, RenderTexture target, bool copyToBackBuffer = false)
        {
            INTERNAL_blit(material, target, copyToBackBuffer);
        }

        [MethodImpl(MethodImplOptions.InternalCall)]
        private static extern void INTERNAL_blit(Material material, RenderTexture target, bool copyToBackBuffer);
    }
}

using System;
using System.Runtime.CompilerServices;

namespace FalcoEngine
{
    public class RenderTexture : Object, IDisposable
    {
        /// <summary>
        /// Render texture type
        /// </summary>
        public enum TextureType { ColorWithDepth, ColorOnly }

        /// <summary>Storage format. HDR preserves linear scene values above one.</summary>
        public enum ColorFormat { LDR = 0, HDR = 1 }

        internal RenderTexture() { }

        /// <summary>
        /// Create new render texture
        /// </summary>
        /// <param name="w">Width</param>
        /// <param name="h">Height</param>
        public RenderTexture(int w, int h)
        {
            INTERNAL_constructor(w, h, (int)TextureType.ColorWithDepth);
        }

        /// <summary>
        /// Create new render texture
        /// </summary>
        /// <param name="w">Width</param>
        /// <param name="h">Height</param>
        /// <param name="textureType">Texture type</param>
        public RenderTexture(int w, int h, TextureType textureType)
        {
            INTERNAL_constructor(w, h, (int)textureType);
        }

        /// <summary>
        /// Create a render texture with explicit color precision. Use HDR for
        /// OnRenderImage intermediates: scene RGB is linear and presentation
        /// applies exposure, tonemapping and sRGB encoding after the script chain.
        /// </summary>
        public RenderTexture(int w, int h, TextureType textureType, ColorFormat format)
        {
            if (w <= 0) throw new ArgumentOutOfRangeException("w");
            if (h <= 0) throw new ArgumentOutOfRangeException("h");
            if (textureType != TextureType.ColorWithDepth && textureType != TextureType.ColorOnly)
                throw new ArgumentOutOfRangeException("textureType");
            if (format != ColorFormat.LDR && format != ColorFormat.HDR)
                throw new ArgumentOutOfRangeException("format");
            if (!INTERNAL_constructorFormat(w, h, (int)textureType, (int)format))
                throw new InvalidOperationException("Render texture allocation failed; see engine log for format or size limits");
        }

        /// <summary>Color storage format; Reset preserves it.</summary>
        public ColorFormat colorFormat { [MethodImpl(MethodImplOptions.InternalCall)] get; }

        /// <summary>Whether all requested render attachments are currently available.</summary>
        public bool isValid { [MethodImpl(MethodImplOptions.InternalCall)] get; }

        [MethodImpl(MethodImplOptions.InternalCall)]
        private extern bool INTERNAL_constructorFormat(int w, int h, int textureType, int format);

        /// <summary>
        /// Recreate this render texture with new width and height
        /// </summary>
        /// <param name="w">New width</param>
        /// <param name="h">New height</param>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public extern void Reset(int w, int h);

        /// <summary>
        /// Get texture containing a color data
        /// </summary>
        public Texture colorTexture { [MethodImpl(MethodImplOptions.InternalCall)] get; }

        /// <summary>
        /// Get texture containing a depth data
        /// </summary>
        public Texture depthTexture { [MethodImpl(MethodImplOptions.InternalCall)] get; }

        /// <summary>
        /// Get width of this render texture
        /// </summary>
        public int width { [MethodImpl(MethodImplOptions.InternalCall)] get; }

        /// <summary>
        /// Get height of this render texture
        /// </summary>
        public int height { [MethodImpl(MethodImplOptions.InternalCall)] get; }

        [MethodImpl(MethodImplOptions.InternalCall)]
        private extern void INTERNAL_constructor(int w, int h, int textureType);

        /// <summary>
        /// Delete unmanaged resources for this render texture used by engine
        /// </summary>
        public void Dispose()
        {
            INTERNAL_dispose();
        }

        [MethodImpl(MethodImplOptions.InternalCall)]
        private extern void INTERNAL_dispose();
    }
}
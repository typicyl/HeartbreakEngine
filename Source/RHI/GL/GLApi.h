// RHI/GL/GLApi.h - minimal OpenGL 2.0+ entry-point loader.
//
// Windows ships only the GL 1.1 import library (opengl32.lib / <GL/gl.h>); every
// entry point from GL 2.0 onward (shaders, VBOs, VAOs, FBOs, UBOs, ...) must be
// fetched at runtime via wglGetProcAddress once a context is current. Rather than
// pull in glad/GLEW (not vendored, and the build is offline), this declares just
// the functions the GL backend uses, via an X-macro so the loader stays in lockstep
// with the declarations. The 1.1 calls (glClear/glViewport/glDrawElements/...) come
// straight from <GL/gl.h> and need no loading.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>

#include <cstddef>
#include <cstdint>

namespace hbe::rhi::gl {

// Types absent from the 1.1 header.
using GLchar     = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr   = std::ptrdiff_t;
using GLuint64   = std::uint64_t;
using GLDEBUGPROC = void(APIENTRY*)(GLenum source, GLenum type, GLuint id,
                                    GLenum severity, GLsizei length,
                                    const GLchar* message, const void* userParam);

// Enum constants beyond GL 1.1 (each guarded - a few leak in from some <GL/gl.h>).
#ifndef GL_FRAGMENT_SHADER
#  define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#  define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#  define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#  define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#  define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_ARRAY_BUFFER
#  define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#  define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_UNIFORM_BUFFER
#  define GL_UNIFORM_BUFFER 0x8A11
#endif
#ifndef GL_STATIC_DRAW
#  define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#  define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_TEXTURE0
#  define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_FRAMEBUFFER
#  define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#  define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#  define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#  define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_RGBA8
#  define GL_RGBA8 0x8058
#endif
#ifndef GL_RGBA16F
#  define GL_RGBA16F 0x881A
#endif
#ifndef GL_RGBA32F
#  define GL_RGBA32F 0x8814
#endif
#ifndef GL_SRGB8_ALPHA8
#  define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_BGRA
#  define GL_BGRA 0x80E1
#endif
#ifndef GL_HALF_FLOAT
#  define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_DEPTH_COMPONENT32F
#  define GL_DEPTH_COMPONENT32F 0x8CAC
#endif
#ifndef GL_CLAMP_TO_EDGE
#  define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#  define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
#  define GL_LINEAR_MIPMAP_LINEAR 0x2703
#endif
#ifndef GL_LOWER_LEFT
#  define GL_LOWER_LEFT 0x8CA1
#endif
#ifndef GL_ZERO_TO_ONE
#  define GL_ZERO_TO_ONE 0x935F
#endif
#ifndef GL_MULTISAMPLE
#  define GL_MULTISAMPLE 0x809D
#endif
#ifndef GL_DEBUG_OUTPUT
#  define GL_DEBUG_OUTPUT 0x92E0
#endif
#ifndef GL_DEBUG_OUTPUT_SYNCHRONOUS
#  define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
#endif
#ifndef GL_DEBUG_SEVERITY_NOTIFICATION
#  define GL_DEBUG_SEVERITY_NOTIFICATION 0x826B
#endif

// The functions the backend uses. X(returnType, name, parenthesizedParams).
#define HBE_GL_FUNCTIONS(X)                                                                       \
    X(GLuint, glCreateShader, (GLenum type))                                                      \
    X(void,   glDeleteShader, (GLuint shader))                                                    \
    X(void,   glShaderSource, (GLuint shader, GLsizei count, const GLchar* const* str, const GLint* len)) \
    X(void,   glCompileShader, (GLuint shader))                                                   \
    X(void,   glGetShaderiv, (GLuint shader, GLenum pname, GLint* params))                        \
    X(void,   glGetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* log)) \
    X(GLuint, glCreateProgram, (void))                                                            \
    X(void,   glDeleteProgram, (GLuint program))                                                  \
    X(void,   glAttachShader, (GLuint program, GLuint shader))                                     \
    X(void,   glLinkProgram, (GLuint program))                                                    \
    X(void,   glGetProgramiv, (GLuint program, GLenum pname, GLint* params))                      \
    X(void,   glGetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei* length, GLchar* log)) \
    X(void,   glUseProgram, (GLuint program))                                                     \
    X(GLint,  glGetUniformLocation, (GLuint program, const GLchar* name))                         \
    X(void,   glUniform1i, (GLint location, GLint v0))                                            \
    X(void,   glUniform1f, (GLint location, GLfloat v0))                                          \
    X(void,   glUniform1ui, (GLint location, GLuint v0))                                          \
    X(void,   glUniform3fv, (GLint location, GLsizei count, const GLfloat* value))               \
    X(void,   glUniform4fv, (GLint location, GLsizei count, const GLfloat* value))               \
    X(void,   glUniformMatrix3fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)) \
    X(void,   glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)) \
    X(GLuint, glGetUniformBlockIndex, (GLuint program, const GLchar* name))                       \
    X(void,   glUniformBlockBinding, (GLuint program, GLuint index, GLuint binding))              \
    X(void,   glGenBuffers, (GLsizei n, GLuint* buffers))                                         \
    X(void,   glDeleteBuffers, (GLsizei n, const GLuint* buffers))                                \
    X(void,   glBindBuffer, (GLenum target, GLuint buffer))                                       \
    X(void,   glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))     \
    X(void,   glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data)) \
    X(void,   glBindBufferBase, (GLenum target, GLuint index, GLuint buffer))                     \
    X(void,   glGenVertexArrays, (GLsizei n, GLuint* arrays))                                     \
    X(void,   glDeleteVertexArrays, (GLsizei n, const GLuint* arrays))                            \
    X(void,   glBindVertexArray, (GLuint array))                                                  \
    X(void,   glEnableVertexAttribArray, (GLuint index))                                          \
    X(void,   glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer)) \
    X(void,   glVertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer)) \
    X(void,   glActiveTexture, (GLenum texture))                                                  \
    X(void,   glGenerateMipmap, (GLenum target))                                                  \
    X(void,   glGenSamplers, (GLsizei n, GLuint* samplers))                                        \
    X(void,   glDeleteSamplers, (GLsizei n, const GLuint* samplers))                               \
    X(void,   glBindSampler, (GLuint unit, GLuint sampler))                                        \
    X(void,   glSamplerParameteri, (GLuint sampler, GLenum pname, GLint param))                    \
    X(void,   glGenFramebuffers, (GLsizei n, GLuint* framebuffers))                               \
    X(void,   glDeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))                      \
    X(void,   glBindFramebuffer, (GLenum target, GLuint framebuffer))                             \
    X(void,   glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                          \
    X(void,   glDrawBuffers, (GLsizei n, const GLenum* bufs))                                     \
    X(void,   glClipControl, (GLenum origin, GLenum depth))                                       \
    X(void,   glDebugMessageCallback, (GLDEBUGPROC callback, const void* userParam))

// Declare a typedef + global pointer for each function.
#define HBE_GL_DECL(ret, name, params)              \
    using PFN_##name = ret(APIENTRY*) params;       \
    extern PFN_##name name;
HBE_GL_FUNCTIONS(HBE_GL_DECL)
#undef HBE_GL_DECL

// Loads every entry point above (a context must be current). Returns false and
// logs if any required function is missing; glDebugMessageCallback is optional.
bool LoadGLFunctions();

} // namespace hbe::rhi::gl

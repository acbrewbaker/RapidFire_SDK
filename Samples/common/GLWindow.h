//
// Copyright (c) 2016 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#pragma once

#include <string>

#include <GL/glew.h>
#include <GL/wglew.h>

class GLWindow
{
public:

    GLWindow(const std::string& strWindowName, unsigned int uiWidth, unsigned int uiHeight, unsigned int uiPosX, unsigned int uiPosY, bool bFullScreen);

    GLWindow(GLWindow&& other);

    virtual ~GLWindow();

    void    open() const;
    void    close() const;

    void    makeCurrent() const;
    void    releaseContext() const;

    void    resize(unsigned int w, unsigned int h);

    operator bool()     const            { return m_bWindowCreated; }

    HDC     getDC()     const            { return m_hDC;   }
    HWND    getWindow() const            { return m_hWND;  }
    HGLRC   getGLRC()   const            { return m_hGLRC; }

    unsigned int getWidth()  const       { return m_uiWidth; }
    unsigned int getHeight() const       { return m_uiHeight; }

private:

    bool         create();

    HDC                     m_hDC;
    HWND                    m_hWND;
    HGLRC                   m_hGLRC;

    std::string const       m_strWindowName;

    bool                    m_bWindowCreated;
    bool                    m_bFullScreen;
    unsigned int            m_uiWidth;
    unsigned int            m_uiHeight;
    unsigned int const      m_uiPosX;
    unsigned int const      m_uiPosY;

    GLWindow(GLWindow const& w);

    GLWindow operator=(GLWindow const rhs);
};
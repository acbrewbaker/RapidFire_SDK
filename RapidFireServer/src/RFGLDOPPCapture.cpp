/*****************************************************************************
* Copyright (C) 2013 Advanced Micro Devices, Inc.
* All rights reserved.
*
* This software is provided by the copyright holders and contributors "As is"
* And any express or implied warranties, including, but not limited to, the
* implied warranties of merchantability, non-infringement, and fitness for a
* particular purpose are disclaimed. In no event shall the copyright holder or
* contributors be liable for any direct, indirect, incidental, special,
* exemplary, or consequential damages (including, but not limited to,
* procurement of substitute goods or services; loss of use, data, or profits;
* or business interruption) however caused and on any theory of liability,
* whether in contract, strict liability, or tort (including negligence or
* otherwise) arising in any way out of the use of this software, even if
* advised of the possibility of such damage.
*****************************************************************************/
#include "RFGLDOPPCapture.h"

#define _USE_MATH_DEFINES
#include <math.h>

#include <GL/glew.h>

#include "RFError.h"
#include "RFGLShader.h"
#include "RFLock.h"

#define GL_WAIT_FOR_PREVIOUS_VSYNC 0x931C

typedef GLuint(APIENTRY* PFNWGLGETDESKTOPTEXTUREAMD)(void);
typedef void   (APIENTRY* PFNWGLENABLEPOSTPROCESSAMD)(bool enable);
typedef GLuint(APIENTRY* WGLGENPRESENTTEXTUREAMD)(void);
typedef GLboolean(APIENTRY* WGLDESKTOPTARGETAMD)(GLuint);
typedef GLuint(APIENTRY* PFNWGLPRESENTTEXTURETOVIDEOAMD)(GLuint presentTexture, const GLuint* attrib_list);

PFNWGLGETDESKTOPTEXTUREAMD      wglGetDesktopTextureAMD;
PFNWGLENABLEPOSTPROCESSAMD      wglEnablePostProcessAMD;
PFNWGLPRESENTTEXTURETOVIDEOAMD  wglPresentTextureToVideoAMD;
WGLDESKTOPTARGETAMD             wglDesktopTargetAMD;
WGLGENPRESENTTEXTUREAMD         wglGenPresentTextureAMD;

#define GET_PROC(xx)                                        \
    {                                                       \
        void **x = reinterpret_cast<void**>(&xx);           \
        *x = static_cast<void*>(wglGetProcAddress(#xx));    \
        if (*x == nullptr) {                                \
            return false;                                   \
        }                                                   \
    }

// Global lock that is used to make sure the GL operations after wglDesktopTarget don't get interrupted.
// A second thread could call wglDesktopTarget and this would lead to artifacts. The desktop texture
// needs to be rendered into the FBO without beeing interrupted by another desktop session.
static RFLock g_GlobalDOPPLock;

GLDOPPCapture::GLDOPPCapture(unsigned int uiDesktop, DOPPDrvInterface* pDrv)
    : m_uiDesktopTexture(0)
    , m_uiDesktopId(uiDesktop)
    , m_uiNumTargets(2)
    , m_uiDesktopWidth(0)
    , m_uiDesktopHeight(0)
    , m_uiPresentWidth(0)
    , m_uiPresentHeight(0)
    , m_pShader(nullptr)
    , m_uiBaseMap(0)
    , m_uiVertexBuffer(0)
    , m_uiVertexArray(0)
    , m_pFBO(nullptr)
    , m_pTexture(nullptr)
    , m_bTrackDesktopChanges(false)
    , m_bBlocking(false)
    , m_pDOPPDrvInterface(pDrv)
{
    if (!m_pDOPPDrvInterface)
    {
        throw std::runtime_error("DOPP no driver ineterface");
    }

    if (m_pDOPPDrvInterface->getDoppState() == false)
    {
        // Try to enable DOPP. If succeeded DOPP will be disabled when the DOPPDrvInterface instance
        // is deleted. No explicit disabling is required.
        m_pDOPPDrvInterface->enableDopp();

        if (!m_pDOPPDrvInterface->getDoppState())
        {
            throw std::runtime_error("DOPP not enabled");
        }
    }

    // Event 0 is signaled by DOPP, Event 1 is used to unblock notification loop.
    m_hDesktopEvent[0] = NULL;
    m_hDesktopEvent[1] = NULL;

    m_bDesktopChanged = false;
}


GLDOPPCapture::~GLDOPPCapture()
{
    wglEnablePostProcessAMD(false);
    HGLRC glrc = wglGetCurrentContext();

    // Make sure we still have a valid context.
    if (glrc)
    {
        if (m_pShader)
        {
            delete m_pShader;
        }

        if (m_uiDesktopTexture)
        {
            glDeleteTextures(1, &m_uiDesktopTexture);
        }

        if (m_pFBO)
        {
            glDeleteFramebuffers(m_uiNumTargets, m_pFBO);
        }

        if (m_pTexture)
        {
            glDeleteTextures(m_uiNumTargets, m_pTexture);
        }

        if (m_uiVertexBuffer)
        {
            glDeleteBuffers(1, &m_uiVertexBuffer);
        }

        if (m_uiVertexArray)
        {
            glDeleteVertexArrays(1, &m_uiVertexArray);
        }
    }
    else
    {
        RF_Error(RF_STATUS_OPENGL_FAIL, "No more valid context whe ndeleting DOPP Capture");
    }

    if (m_pFBO)
    {
        delete[] m_pFBO;
        m_pFBO = nullptr;
    }

    if (m_pTexture)
    {
        delete[] m_pTexture;
        m_pTexture = nullptr;
    }

    // Set changes tracking to false. This will cause the notifcation thread to stop.
    m_bTrackDesktopChanges = false;

    // Release desktop change notification event to unblock the notification thread.
    if (m_hDesktopEvent[0])
    {
        SetEvent(m_hDesktopEvent[0]);
        Sleep(0);
    }

    if (m_hDesktopEvent[1])
    {
        CloseHandle(m_hDesktopEvent[1]);
        m_hDesktopEvent[1] = NULL;
    }

    // Terminate the notification thread.
    if (m_NotificationThread.joinable())
    {
        m_NotificationThread.join();
    }

    // Delete the desktop notification event.
    if (m_hDesktopEvent[0])
    {
        m_pDOPPDrvInterface->deleteDOPPEvent(m_hDesktopEvent[0]);

        m_hDesktopEvent[0] = NULL;
    }
}


RFStatus GLDOPPCapture::initDOPP(unsigned int uiPresentWidth, unsigned int uiPresentHeight, float fRotation, bool bTrackDesktopChanges, bool bBlocking)
{
    RFReadWriteAccess doppLock(&g_GlobalDOPPLock);

    HGLRC glrc = wglGetCurrentContext();

    if (!glrc)
    {
        return RF_STATUS_OPENGL_FAIL;
    }

    if (uiPresentWidth <= 0 || uiPresentHeight <= 0)
    {
        return RF_STATUS_INVALID_DIMENSION;
    }

    m_uiPresentWidth = uiPresentWidth;
    m_uiPresentHeight = uiPresentHeight;

    if (!setupDOPPExtension())
    {
        return RF_STATUS_DOPP_FAIL;
    }

    // Select the Desktop to be processed. ID is the same as seen in CCC.
    if (!wglDesktopTargetAMD(m_uiDesktopId))
    {
        return RF_STATUS_INVALID_DESKTOP_ID;
    }

    m_uiDesktopTexture = wglGetDesktopTextureAMD();
    glBindTexture(GL_TEXTURE_2D, m_uiDesktopTexture);

    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, reinterpret_cast<GLint*>(&m_uiDesktopWidth));
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, reinterpret_cast<GLint*>(&m_uiDesktopHeight));

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    if (!initEffect())
    {
        return RF_STATUS_DOPP_FAIL;
    }

    if (!createRenderTargets())
    {
        return RF_STATUS_DOPP_FAIL;
    }

    createQuad(fRotation);

    m_bTrackDesktopChanges = bTrackDesktopChanges;
    m_bBlocking = bBlocking;


    if (m_bBlocking && !m_bTrackDesktopChanges)
    {
        // If a blocking call is requested, we need to track desktop changes.
        m_bTrackDesktopChanges = true;
    }

    if (m_bTrackDesktopChanges)
    {
        // Create the event to get notifications on Desktop changes.
        m_hDesktopEvent[0] = m_pDOPPDrvInterface->createDOPPEvent(DOPPEventType::DOPP_DESKOTOP_EVENT);

        if (!m_hDesktopEvent[0])
        {
            // If registration fails, indicate that no changes are tracked, desktop capturing is still functinal.
            m_bTrackDesktopChanges = false;
        }
        else
        {
            // Create the event that can be signaled to unblock processDesktop if the blocking option is set.
            // A separate event is used to differentiate between a notifaction which triggers the rendering
            // of the desktop texture and a release call which will only unblock but won't generate a new 
            // desktop image.
            m_hDesktopEvent[1] = CreateEvent(NULL, FALSE, FALSE, NULL);
        }
    }

    // If changes are tracked and processDesktop is non-blcking we need a notificatin thread.
    if (m_bTrackDesktopChanges && !m_bBlocking)
    {
        m_NotificationThread = std::thread(&GLDOPPCapture::notificationLoop, this);
    }

    return RF_STATUS_OK;
}


bool GLDOPPCapture::createRenderTargets()
{
    if (m_pFBO != nullptr || m_pTexture != nullptr)
    {
        return false;
    }

    m_pFBO = new GLuint[m_uiNumTargets];
    m_pTexture = new GLuint[m_uiNumTargets];

    glGenFramebuffers(m_uiNumTargets, m_pFBO);
    glGenTextures(m_uiNumTargets, m_pTexture);

    bool bFBStatus = true;

    for (unsigned int i = 0; i < m_uiNumTargets; ++i)
    {
        glBindTexture(GL_TEXTURE_2D, m_pTexture[i]);

        // WORKAROUND to avoid conflicst with AMF avoid using GL_RGBA8.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_uiPresentWidth, m_uiPresentHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, m_pFBO[i]);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_pTexture[i], 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            bFBStatus = false;
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return bFBStatus;
}


RFStatus GLDOPPCapture::resizeDesktopTexture()
{
    if (m_uiDesktopId > 0)
    {
        RFReadWriteAccess doppLock(&g_GlobalDOPPLock);

        if (m_uiDesktopTexture)
        {
            glDeleteTextures(1, &m_uiDesktopTexture);
        }

        // The resize might happen after a display topology change -> we could fail getting
        // a desktop texture for this m_uiDesktopId.
        if (!wglDesktopTargetAMD(m_uiDesktopId))
        {
            return RF_STATUS_INVALID_DESKTOP_ID;
        }

        m_uiDesktopTexture = wglGetDesktopTextureAMD();

        glBindTexture(GL_TEXTURE_2D, m_uiDesktopTexture);

        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Get the size of the desktop. Usually these are the same values as returned by GetSystemMetrics(SM_CXSCREEN)
        // and GetSystemMetrics(SM_CYSCREEN). In some cases they might differ, e.g. if a rotated desktop is used.
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, reinterpret_cast<GLint*>(&m_uiDesktopWidth));
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, reinterpret_cast<GLint*>(&m_uiDesktopHeight));

        glBindTexture(GL_TEXTURE_2D, 0);

        return RF_STATUS_OK;
    }

    return RF_STATUS_INVALID_DESKTOP_ID;
}


RFStatus GLDOPPCapture::resizePresentTexture(unsigned int uiPresentWidth, unsigned int uiPresentHeight)
{
    if (m_pTexture)
    {
        glDeleteTextures(m_uiNumTargets, m_pTexture);

        delete[] m_pTexture;
        m_pTexture = nullptr;
    }

    if (m_pFBO)
    {
        glDeleteFramebuffers(m_uiNumTargets, m_pFBO);

        delete[] m_pFBO;
        m_pFBO = nullptr;
    }


    m_uiPresentWidth = uiPresentWidth;
    m_uiPresentHeight = uiPresentHeight;

    if (!createRenderTargets())
    {
        return RF_STATUS_OPENGL_FAIL;
    }

    return RF_STATUS_OK;
}


bool GLDOPPCapture::releaseEvent()
{
    if (m_bBlocking)
    {
        SetEvent(m_hDesktopEvent[1]);
        Sleep(0);

        return true;
    }

    return false;
}


bool GLDOPPCapture::initEffect()
{
    if (m_pShader)
    {
        delete m_pShader;
    }

    const char* strVertexShader =
    {
        "#version 420                                                \n"
        "                                                            \n"
        "layout(location = 0) in vec4 inVertex;                      \n"
        "layout(location = 4) in vec2 inTexCoord;                    \n"
        "                                                            \n"
        "varying vec2 Texcoord;                                      \n"
        "                                                            \n"
        "void main(void)                                             \n"
        "{                                                           \n"
        "    gl_Position = inVertex;								 \n"
        "    Texcoord    = inTexCoord;                               \n"
        "}                                                           \n"
    };

    const char* strFragmentShader =
    {
        "#version 420                                                        \n"
        "                                                                    \n"
        "uniform sampler2D baseMap;                                          \n"
        "                                                                    \n"
        "varying vec2 Texcoord;                                              \n"
        "                                                                    \n"
        "void main(void)                                                     \n"
        "{                                                                   \n"
        "    vec4 texColor = texture2D(baseMap, Texcoord);                   \n"
        "                                                                    \n"
        "    gl_FragColor = vec4(texColor.r, texColor.g, texColor.b, 1.0f);  \n"
        "}                                                                   \n"
    };

    m_pShader = new (std::nothrow)GLShader;

    if (!m_pShader)
    {
        return false;
    }

    if (!m_pShader->createShaderFromString(strVertexShader, GL_VERTEX_SHADER))
    {
        return false;
    }

    if (!m_pShader->createShaderFromString(strFragmentShader, GL_FRAGMENT_SHADER))
    {
        return false;
    }

    if (!m_pShader->buildProgram())
    {
        return false;
    }

    m_pShader->bind();

    m_uiBaseMap = glGetUniformLocation(m_pShader->getProgram(), "baseMap");

    m_pShader->unbind();

    return true;
}


bool GLDOPPCapture::processDesktop(unsigned int idx)
{
    if (idx >= m_uiNumTargets)
    {
        idx = 0;
    }

    if (m_bTrackDesktopChanges)
    {
        if (m_bBlocking)
        {
            DWORD dwResult = WaitForMultipleObjects(2, m_hDesktopEvent, FALSE, INFINITE);

            if ((dwResult - WAIT_OBJECT_0) == 1)
            {
                // Thread was unblocked by internal event not by DOPP.
                return false;
            }
        }
        else if (!m_bDesktopChanged.load())
        {
            return false;
        }
    }

    {
        // GLOBAL LOCK: The operations of selecting the desktop and rendering the desktop texture
        // into the FBO must not be interrupted. Otherwise another thread may select another
        // Desktop by calling wglDesktopTarget while the previous one was not completely processed.
        RFReadWriteAccess doppLock(&g_GlobalDOPPLock);

        glBindFramebuffer(GL_FRAMEBUFFER, m_pFBO[idx]);

        int pVP[4];

        // Store old VP just in case the calling app used OpenGL as well.
        glGetIntegerv(GL_VIEWPORT, pVP);

        glViewport(0, 0, m_uiPresentWidth, m_uiPresentHeight);

        wglDesktopTargetAMD(m_uiDesktopId);

        m_pShader->bind();

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_uiDesktopTexture);

        glUniform1i(m_uiBaseMap, 1);

        glBindVertexArray(m_uiVertexArray);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);

        m_pShader->unbind();

        glBindTexture(GL_TEXTURE_2D, 0);

        glActiveTexture(GL_TEXTURE0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Restore original viewport.
        glViewport(pVP[0], pVP[1], pVP[2], pVP[3]);

        m_bDesktopChanged = false;

        glFinish();
    }

    return true;
}


unsigned int GLDOPPCapture::getFramebufferTex(unsigned int idx) const
{
    if (m_uiNumTargets > 0 && idx < m_uiNumTargets && m_pTexture)
    {
        return m_pTexture[idx];
    }

    return 0;
}


void GLDOPPCapture::createQuad(float fRotation)
{
    const float phi = (static_cast<float>(M_PI) * fRotation) / 180.0f;

    const float vec[] = { -cosf(phi) - sinf(phi),  -sinf(phi) + cosf(phi),  0.0f, 1.0f,   // -1.0f,  1.0f, 0.0f, 1.0f,
                          -cosf(phi) + sinf(phi),  -sinf(phi) - cosf(phi),  0.0f, 1.0f,   // -1.0f, -1.0f, 0.0f, 1.0f,   
                           cosf(phi) - sinf(phi),   sinf(phi) + cosf(phi),  0.0f, 1.0f,   //  1.0f,  1.0f, 0.0f, 1.0f,  
                           cosf(phi) + sinf(phi),   sinf(phi) - cosf(phi),  0.0f, 1.0f }; //  1.0f, -1.0f, 0.0f, 1.0f  

    const float tex[] = { 0.0f, 1.0f,   0.0f,  0.0f,   1.0f, 1.0f,   1.0f, 0.0f };

    glGenVertexArrays(1, &m_uiVertexArray);
    glBindVertexArray(m_uiVertexArray);

    glGenBuffers(1, &m_uiVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, m_uiVertexBuffer);

    glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), vec, GL_STATIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 16 * sizeof(float), 8 * sizeof(float), tex);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(4);

    glVertexAttribPointer((GLuint)0, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glVertexAttribPointer((GLuint)4, 2, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<void*>(16 * sizeof(float)));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}


bool GLDOPPCapture::setupDOPPExtension()
{
    GET_PROC(wglGetDesktopTextureAMD);
    GET_PROC(wglEnablePostProcessAMD);
    GET_PROC(wglPresentTextureToVideoAMD);
    GET_PROC(wglDesktopTargetAMD);
    GET_PROC(wglGenPresentTextureAMD);

    return true;
}


void GLDOPPCapture::notificationLoop()
{
    while (m_bTrackDesktopChanges)
    {
        DWORD dwResult = WaitForMultipleObjects(2, m_hDesktopEvent, FALSE, INFINITE);

        if ((dwResult - WAIT_OBJECT_0) == 0)
        {
            m_bDesktopChanged.store(true);
        }
    }
}
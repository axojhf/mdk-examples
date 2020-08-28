/*
 * Copyright (c) 2020 WangBin <wbsecg1 at gmail.com>
 * MDK SDK in QtQuick RHI
 */
#include "VideoTextureItem.h"
#include <QtGui/QScreen>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGTextureProvider>
#include <QtQuick/QSGSimpleTextureNode>
#if (__APPLE__+0)
#include <Metal/Metal.h>
#endif
#if (_WIN32+0)
#include <d3d11.h>
#include <wrl/client.h>
using namespace Microsoft::WRL; //ComPtr
#endif
#if QT_CONFIG(opengl)
#include <QOpenGLFramebufferObject>
#endif
#include <QVulkanInstance>
#include <QVulkanFunctions>

#include "mdk/Player.h"
#include "mdk/RenderAPI.h"
using namespace std;

//! [1]
class VideoTextureNode : public QSGTextureProvider, public QSGSimpleTextureNode
{
    Q_OBJECT
public:
    VideoTextureNode(VideoTextureItem *item);
    ~VideoTextureNode();

    QSGTexture *texture() const override;

    void sync();
//! [1]
private slots:
    void render();

private:
    QQuickItem *m_item;
    QQuickWindow *m_window;
    QSize m_size;
    qreal m_dpr;
#if (__APPLE__+0)
    id<MTLTexture> m_texture_mtl = nil;
#endif
#if QT_CONFIG(opengl)
    unique_ptr<QOpenGLFramebufferObject> fbo_gl;
#endif
#if (_WIN32+0)
    ComPtr<ID3D11Texture2D> m_texture_d3d11;
#endif
#if (VK_VERSION_1_0+0) && QT_CONFIG(vulkan)
    bool buildTexture(const QSize &size);
    void freeTexture();
    bool createRenderPass();

    VkImage m_texture_vk = VK_NULL_HANDLE;
    VkDeviceMemory m_textureMemory = VK_NULL_HANDLE;
    VkFramebuffer m_textureFramebuffer = VK_NULL_HANDLE;
    VkImageView m_textureView = VK_NULL_HANDLE;

    VkPhysicalDevice m_physDev = VK_NULL_HANDLE;
    VkDevice m_dev = VK_NULL_HANDLE;
    QVulkanDeviceFunctions *m_devFuncs = nullptr;
    QVulkanFunctions *m_funcs = nullptr;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
#endif

    std::weak_ptr<Player> m_player;
};

VideoTextureItem::VideoTextureItem()
{
    setFlag(ItemHasContents, true);
    m_player = make_shared<Player>();
    //m_player->setVideoDecoders({"VT", "MFT:d3d=11", "VAAPI", "FFmpeg"});
    m_player->setRenderCallback([=](void *){
        QMetaObject::invokeMethod(this, "update");
    });
}

VideoTextureItem::~VideoTextureItem() = default;

// The beauty of using a true QSGNode: no need for complicated cleanup
// arrangements, unlike in other examples like metalunderqml, because the
// scenegraph will handle destroying the node at the appropriate time.
void VideoTextureItem::invalidateSceneGraph() // called on the render thread when the scenegraph is invalidated
{
    m_node = nullptr;
}

void VideoTextureItem::releaseResources() // called on the gui thread if the item is removed from scene
{
    m_node = nullptr;
}

//! [2]
QSGNode *VideoTextureItem::updatePaintNode(QSGNode *node, UpdatePaintNodeData *)
{
    auto n = static_cast<VideoTextureNode*>(node);

    if (!n && (width() <= 0 || height() <= 0))
        return nullptr;

    if (!n) {
        m_node = new VideoTextureNode(this);
        n = m_node;
    }

    m_node->sync();

    n->setTextureCoordinatesTransform(QSGSimpleTextureNode::NoTransform);
    n->setFiltering(QSGTexture::Linear);
    n->setRect(0, 0, width(), height());

    window()->update(); // ensure getting to beforeRendering() at some point

    return n;
}
//! [2]

void VideoTextureItem::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChanged(newGeometry, oldGeometry);

    if (newGeometry.size() != oldGeometry.size())
        update();
}

void VideoTextureItem::setSource(const QString & s)
{
    m_player->setMedia(s.toLocal8Bit().data());
    m_source = s;
    emit sourceChanged();
}

void VideoTextureItem::play()
{
    m_player->setState(PlaybackState::Playing);
}

//! [3]
VideoTextureNode::VideoTextureNode(VideoTextureItem *item)
    : m_item(item)
    , m_player(item->m_player)
{
    m_window = m_item->window();
    connect(m_window, &QQuickWindow::beforeRendering, this, &VideoTextureNode::render);
    connect(m_window, &QQuickWindow::screenChanged, this, [this]() {
        if (m_window->effectiveDevicePixelRatio() != m_dpr)
            m_item->update();
    });
//! [3]
}

VideoTextureNode::~VideoTextureNode()
{
    delete texture();
    // release gfx resources
#if QT_CONFIG(opengl)
    fbo_gl.reset();
#endif
#if (VK_VERSION_1_0+0) && QT_CONFIG(vulkan)
    if (m_devFuncs) {
        m_devFuncs->vkDestroyRenderPass(m_dev, m_renderPass, nullptr);
        freeTexture();
    }
#endif
    qDebug("renderer destroyed");
}

QSGTexture *VideoTextureNode::texture() const
{
    return QSGSimpleTextureNode::texture();
}

void VideoTextureNode::sync()
{
    m_dpr = m_window->effectiveDevicePixelRatio();
    const QSizeF newSize = m_item->size() * m_dpr;
    bool needsNew = false;

    if (!texture())
        needsNew = true;

    if (newSize != m_size) {
        needsNew = true;
        m_size = {qRound(newSize.width()), qRound(newSize.height())};
    }

    if (!needsNew)
        return;

    delete texture();

    auto player = m_player.lock();
    if (!player)
        return;
    QSGRendererInterface *rif = m_window->rendererInterface();
    void* nativeObj = nullptr;
    int nativeLayout = 0;
    switch (rif->graphicsApi()) {
    case QSGRendererInterface::OpenGL:
        Q_FALLTHROUGH();
    case QSGRendererInterface::OpenGLRhi: { // FIXME: OpenGLRhi does not work
#if QT_CONFIG(opengl)
        fbo_gl.reset(new QOpenGLFramebufferObject(m_size));
        auto tex = fbo_gl->texture();
        nativeObj = (void*)tex;
        GLRenderAPI ra;
        ra.fbo = fbo_gl->handle();
        player->setRenderAPI(&ra);
        player->scale(1.0f, -1.0f); // flip y
#endif
    }
        break;
    case QSGRendererInterface::Direct3D11Rhi: {
#if (_WIN32)
        auto dev = (ID3D11Device*)rif->getResource(m_window, QSGRendererInterface::DeviceResource);
        D3D11_TEXTURE2D_DESC desc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R8G8B8A8_UNORM, m_size.width(), m_size.height(), 1, 1
                                                          , D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET
                                                          , D3D11_USAGE_DEFAULT, 0, 1, 0, 0);
        if (FAILED(dev->CreateTexture2D(&desc, nullptr, &m_texture_d3d11))) {

        }
        nativeObj = m_texture_d3d11.Get();
        D3D11RenderAPI ra;
        ra.rtv = m_texture_d3d11.Get();
        player->setRenderAPI(&ra);
#endif
    }
        break;
    case QSGRendererInterface::MetalRhi: {
#if (__APPLE__+0)
        auto dev = (__bridge id<MTLDevice>)rif->getResource(m_window, QSGRendererInterface::DeviceResource);
        Q_ASSERT(dev);

        MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
        desc.textureType = MTLTextureType2D;
        desc.pixelFormat = MTLPixelFormatRGBA8Unorm;
        desc.width = m_size.width();
        desc.height = m_size.height();
        desc.mipmapLevelCount = 1;
        desc.resourceOptions = MTLResourceStorageModePrivate;
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        m_texture_mtl = [dev newTextureWithDescriptor: desc];
        nativeObj = (__bridge void*)m_texture_mtl;
        MetalRenderAPI ra{};
        ra.texture = nativeObj;
        ra.device = (__bridge void*)dev;
        ra.cmdQueue = rif->getResource(m_window, QSGRendererInterface::CommandQueueResource);
        player->setRenderAPI(&ra);
#endif
    }
    case QSGRendererInterface::VulkanRhi: {
#if (VK_VERSION_1_0+0) && QT_CONFIG(vulkan)
        nativeLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        auto inst = reinterpret_cast<QVulkanInstance *>(rif->getResource(m_window, QSGRendererInterface::VulkanInstanceResource));
        m_physDev = *static_cast<VkPhysicalDevice *>(rif->getResource(m_window, QSGRendererInterface::PhysicalDeviceResource));
        m_dev = *static_cast<VkDevice *>(rif->getResource(m_window, QSGRendererInterface::DeviceResource));
        m_devFuncs = inst->deviceFunctions(m_dev);
        m_funcs = inst->functions();

        createRenderPass();

        freeTexture();
        buildTexture(m_size);
        nativeObj = (void*)m_texture_vk;

        VulkanRenderAPI ra{};
        ra.instance = inst->vkInstance();
        ra.device =m_dev;
        ra.phy_device = m_physDev;
        ra.render_pass = m_renderPass; //
        ra.opaque = this;
        ra.renderTargetSize = [](void* opaque, int* w, int* h) {
            auto node = static_cast<VideoTextureNode*>(opaque);
            *w = node->m_size.width();
            *h = node->m_size.height();
            return 1;
        };
        ra.beginFrame = [](void* opaque, VkImageView* view/* = nullptr*/, VkFramebuffer* fb/*= nullptr*/, VkSemaphore* imgSem/* = nullptr*/){
            auto node = static_cast<VideoTextureNode*>(opaque);
            *fb = node->m_textureFramebuffer;
            return 0;
        };
        ra.currentCommandBuffer = [](void* opaque){
            auto node = static_cast<VideoTextureNode*>(opaque);
            QSGRendererInterface *rif = node->m_window->rendererInterface();
            auto cmdBuf = *static_cast<VkCommandBuffer *>(rif->getResource(node->m_window, QSGRendererInterface::CommandListResource));
            return cmdBuf;
        };
        ra.endFrame = [](void* opaque, VkSemaphore* drawSem/* = nullptr*/){
        };
        player->setRenderAPI(&ra);
#endif
    }
        break;
    default:
        break;
    }
    if (nativeObj) {
        QSGTexture *wrapper = m_window->createTextureFromNativeObject(QQuickWindow::NativeObjectTexture,
                                                                      &nativeObj,
                                                                      nativeLayout,
                                                                      m_size);
        setTexture(wrapper);
    }
    player->setVideoSurfaceSize(m_size.width(), m_size.height());
}

// This is hooked up to beforeRendering() so we can start our own render
// command encoder. If we instead wanted to use the scenegraph's render command
// encoder (targeting the window), it should be connected to
// beforeRenderPassRecording() instead.
//! [6]
void VideoTextureNode::render()
{
    auto player = m_player.lock();
    if (!player)
        return;
    player->renderVideo();
}

#if (VK_VERSION_1_0+0) && QT_CONFIG(vulkan)
bool VideoTextureNode::buildTexture(const QSize &size)
{
    VkImageCreateInfo imageInfo;
    memset(&imageInfo, 0, sizeof(imageInfo));
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = 0;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent.width = uint32_t(size.width());
    imageInfo.extent.height = uint32_t(size.height());
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImage image = VK_NULL_HANDLE;
    if (m_devFuncs->vkCreateImage(m_dev, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        qCritical("VulkanWrapper: failed to create image!");
        return  false;
    }

    m_texture_vk = image;

    VkMemoryRequirements memReq;
    m_devFuncs->vkGetImageMemoryRequirements(m_dev, image, &memReq);

    quint32 memIndex = 0;
    VkPhysicalDeviceMemoryProperties physDevMemProps;
    m_window->vulkanInstance()->functions()->vkGetPhysicalDeviceMemoryProperties(m_physDev, &physDevMemProps);
    for (uint32_t i = 0; i < physDevMemProps.memoryTypeCount; ++i) {
        if (!(memReq.memoryTypeBits & (1 << i)))
            continue;
        memIndex = i;
    }

    VkMemoryAllocateInfo allocInfo = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr,
        memReq.size,
        memIndex
    };

    VkResult err = m_devFuncs->vkAllocateMemory(m_dev, &allocInfo, nullptr, &m_textureMemory);
    if (err != VK_SUCCESS) {
        qWarning("Failed to allocate memory for linear image: %d", err);
        return false;
    }

    err = m_devFuncs->vkBindImageMemory(m_dev, image, m_textureMemory, 0);
    if (err != VK_SUCCESS) {
        qWarning("Failed to bind linear image memory: %d", err);
        return false;
    }

    VkImageViewCreateInfo viewInfo;
    memset(&viewInfo, 0, sizeof(viewInfo));
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageInfo.format;
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_R;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_G;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_B;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_A;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    err = m_devFuncs->vkCreateImageView(m_dev, &viewInfo, nullptr, &m_textureView);
    if (err != VK_SUCCESS) {
        qWarning("Failed to create render target image view: %d", err);
        return false;
    }

    VkFramebufferCreateInfo fbInfo;
    memset(&fbInfo, 0, sizeof(fbInfo));
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &m_textureView;
    fbInfo.width = uint32_t(size.width());
    fbInfo.height = uint32_t(size.height());
    fbInfo.layers = 1;

    err = m_devFuncs->vkCreateFramebuffer(m_dev, &fbInfo, nullptr, &m_textureFramebuffer);
    if (err != VK_SUCCESS) {
        qWarning("Failed to create framebuffer: %d", err);
        return false;
    }
    return true;
}

void VideoTextureNode::freeTexture()
{
    if (m_texture_vk) {
        m_devFuncs->vkDestroyFramebuffer(m_dev, m_textureFramebuffer, nullptr);
        m_textureFramebuffer = VK_NULL_HANDLE;
        m_devFuncs->vkFreeMemory(m_dev, m_textureMemory, nullptr);
        m_textureMemory = VK_NULL_HANDLE;
        m_devFuncs->vkDestroyImageView(m_dev, m_textureView, nullptr);
        m_textureView = VK_NULL_HANDLE;
        m_devFuncs->vkDestroyImage(m_dev, m_texture_vk, nullptr);
        m_texture_vk = VK_NULL_HANDLE;
    }
}



static inline VkDeviceSize aligned(VkDeviceSize v, VkDeviceSize byteAlign)
{
    return (v + byteAlign - 1) & ~(byteAlign - 1);
}

bool VideoTextureNode::createRenderPass()
{
    const VkFormat vkformat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkSampleCountFlagBits samples =  VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentDescription colorAttDesc;
    memset(&colorAttDesc, 0, sizeof(colorAttDesc));
    colorAttDesc.format = vkformat;
    colorAttDesc.samples = samples;
    colorAttDesc.loadOp =  VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttDesc.storeOp =  VK_ATTACHMENT_STORE_OP_STORE;
    colorAttDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttDesc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    const VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpassDesc;
    memset(&subpassDesc, 0, sizeof(subpassDesc));
    subpassDesc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpassDesc.colorAttachmentCount = 1;
    subpassDesc.pColorAttachments = &colorRef;
    subpassDesc.pDepthStencilAttachment = nullptr;
    subpassDesc.pResolveAttachments = nullptr;

    VkRenderPassCreateInfo rpInfo;
    memset(&rpInfo, 0, sizeof(rpInfo));
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &colorAttDesc;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpassDesc;

    VkResult err = m_devFuncs->vkCreateRenderPass(m_dev, &rpInfo, nullptr, &m_renderPass);
    if (err != VK_SUCCESS) {
        qWarning("Failed to create renderpass: %d", err);
        return false;
    }

    return true;
}
#endif //(VK_VERSION_1_0+0) && QT_CONFIG(vulkan)

#include "VideoTextureItem.moc"

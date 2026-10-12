// Dependency/link/plugin smoke test, not an interactive canvas validation.
#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QPdfDocument>
#include <QSslSocket>
#include <QSvgRenderer>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <webp/encode.h>
#include <webp/decode.h>
#include <webp/mux.h>
#include <mz.h>
#include <mz_strm_mem.h>
#include <vulkan/vulkan.h>
#include <zlib.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    try {
        require(argc == 2, "Expected compiled shader path");
        QNetworkAccessManager network;
        require(QSslSocket::supportsSsl(), "Qt TLS backend is unavailable");
        QSvgRenderer svg(QByteArray("<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'><rect width='4' height='4'/></svg>"));
        require(svg.isValid(), "Qt SVG renderer unavailable");
        for (const char* format : { "PNG", "JPEG", "WEBP" }) {
            QImage image(4, 4, QImage::Format_RGB32);
            image.fill(QColor(120, 70, 40));
            QByteArray bytes;
            QBuffer buffer(&bytes);
            require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, format), "Qt image encoder unavailable");
            require(QImage::fromData(bytes, format).size() == image.size(), "Qt image decoder unavailable");
        }
        QPDF pdf;
        pdf.emptyPDF();
        QPDFWriter writer(pdf);
        writer.setOutputMemory();
        writer.write();
        const auto output = writer.getBuffer();
        QByteArray pdfBytes(reinterpret_cast<const char*>(output->getBuffer()), static_cast<qsizetype>(output->getSize()));
        QBuffer pdfBuffer(&pdfBytes);
        require(pdfBuffer.open(QIODevice::ReadOnly), "Could not open PDF buffer");
        QPdfDocument pdfDocument;
        pdfDocument.load(&pdfBuffer);
        require(pdfDocument.error() == QPdfDocument::Error::None, "Qt PDF could not read QPDF output");

        const std::array<unsigned char, 4> pixel { 13, 170, 50, 255 };
        unsigned char* encoded {};
        const auto encodedSize = WebPEncodeLosslessRGBA(pixel.data(), 1, 1, 4, &encoded);
        require(encodedSize != 0, "WebP encoding failed");
        int width {}, height {};
        auto* decoded = WebPDecodeRGBA(encoded, encodedSize, &width, &height);
        const bool exact = decoded && width == 1 && height == 1 && std::memcmp(pixel.data(), decoded, 4) == 0;
        WebPFree(encoded);
        WebPFree(decoded);
        require(exact, "WebP lossless round-trip failed");
        auto* mux = WebPMuxNew();
        require(mux, "WebP mux library unavailable");
        WebPMuxDelete(mux);
        std::array<Bytef, 128> compressed {};
        uLongf compressedSize = static_cast<uLongf>(compressed.size());
        require(compress(compressed.data(), &compressedSize, pixel.data(), static_cast<uLong>(pixel.size())) == Z_OK, "zlib compression failed");
        std::array<Bytef, 4> restored {};
        uLongf restoredSize = static_cast<uLongf>(restored.size());
        require(uncompress(restored.data(), &restoredSize, compressed.data(), compressedSize) == Z_OK
                && restored == pixel, "zlib round-trip failed");
        void* stream = mz_stream_mem_create();
        require(stream, "minizip-ng memory stream unavailable");
        mz_stream_mem_delete(&stream);
        std::cout << "PASS Qt " << qVersion() << " Widgets/PDF/SVG/TLS/image plugins, QPDF, WebP, zlib, minizip-ng\n";

        VkApplicationInfo appInfo { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        appInfo.pApplicationName = "Vulkana dependency probe";
        appInfo.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instanceInfo.pApplicationInfo = &appInfo;
        VkInstance instance {};
        require(vkCreateInstance(&instanceInfo, nullptr, &instance) == VK_SUCCESS, "Vulkan instance creation failed");
        uint32_t count {};
        require(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count, "No Vulkan device");
        std::vector<VkPhysicalDevice> devices(count);
        require(vkEnumeratePhysicalDevices(instance, &count, devices.data()) == VK_SUCCESS, "Vulkan enumeration failed");
        VkPhysicalDevice selected = devices.front();
        for (auto device : devices) {
            VkPhysicalDeviceProperties properties {};
            vkGetPhysicalDeviceProperties(device, &properties);
            std::cout << "Vulkan device: " << properties.deviceName << '\n';
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                selected = device;
        }
        uint32_t queueCount {};
        vkGetPhysicalDeviceQueueFamilyProperties(selected, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(selected, &queueCount, queues.data());
        uint32_t queueIndex = 0;
        while (queueIndex < queueCount && !(queues[queueIndex].queueFlags & VK_QUEUE_COMPUTE_BIT))
            ++queueIndex;
        require(queueIndex < queueCount, "No Vulkan compute queue");
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = queueIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        VkDevice device {};
        require(vkCreateDevice(selected, &deviceInfo, nullptr, &device) == VK_SUCCESS, "Vulkan logical device creation failed");
        QFile file(QString::fromLocal8Bit(argv[1]));
        require(file.open(QIODevice::ReadOnly), "Could not read compiled shader");
        const QByteArray shaderBytes = file.readAll();
        require(!shaderBytes.isEmpty() && shaderBytes.size() % sizeof(uint32_t) == 0, "Invalid SPIR-V size");
        std::vector<uint32_t> words(shaderBytes.size() / sizeof(uint32_t));
        std::memcpy(words.data(), shaderBytes.data(), shaderBytes.size());
        VkShaderModuleCreateInfo shaderInfo { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        shaderInfo.codeSize = shaderBytes.size();
        shaderInfo.pCode = words.data();
        VkShaderModule module {};
        require(vkCreateShaderModule(device, &shaderInfo, nullptr, &module) == VK_SUCCESS, "Vulkan shader module creation failed");
        vkDestroyShaderModule(device, module, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        std::cout << "PASS Vulkan device + actual Vulkana composite shader module (not a rendered frame)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

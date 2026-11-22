#pragma once

#include "utterance.h"
#include <QNetworkAccessManager>
#include <functional>

// Returned observations carry event and segment identifiers. Locators and pixel
// data belong to the transient provider request and never enter the chat prompt.
class ImagePerception
{
public:
    void observe(const RequestMeta &meta, std::function<void(QJsonArray)> done);
private:
    QNetworkAccessManager network;
};

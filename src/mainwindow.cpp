#include "mainwindow.h"
#include "overlay.h"
#include "ui_mainwindow.h"
#include "imageswitcher.h"
#include "logger.h"
#include <QLabel>
#include <QPixmap>
#include <QBitmap>
#include <QKeyEvent>
#include <QGraphicsBlurEffect>
#include <libexif/exif-data.h>
#include <iostream>
#include <QPainter>
#include <QTimer>
#include <QPropertyAnimation>
#include <QRect>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QApplication>
#include <QScreen>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint | Qt::X11BypassWindowManagerHint);

    setAttribute(Qt::WA_AcceptTouchEvents);

    QTimer::singleShot(5, this, SLOT(showFullScreen()));
    //QApplication::setOverrideCursor(Qt::BlankCursor);

    // BUGBUG - with the move to Debian bookworm and the Wayland renderer
    //  the setOverrideCursor call above stopped hiding the cursor. So instead set
    //  and empty image for the window cursor.
    QImage imageBit(1,1, QImage::Format_Mono);
    imageBit.fill(Qt::color0);
    QImage imageMsk(1,1, QImage::Format_Mono);
    imageMsk.fill(Qt::color1);
    QBitmap bitmapBit = QBitmap::fromImage(imageBit);
    QBitmap bitmapMsk = QBitmap::fromImage(imageMsk);
    QCursor cursor = QCursor(bitmapBit, bitmapMsk, 0, 0);
    this->setCursor(cursor);

    QLabel *label = this->findChild<QLabel*>("image");
    setCentralWidget(label);
    label->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    update();

    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen)
    {
      connect(screen, SIGNAL(geometryChanged(QRect)), this, SLOT(checkWindowSize()));
      connect(screen, SIGNAL(orientationChanged(Qt::ScreenOrientation)), this, SLOT(checkWindowSize()));
      screen->setOrientationUpdateMask(Qt::LandscapeOrientation |
                                      Qt::PortraitOrientation |
                                      Qt::InvertedLandscapeOrientation |
                                      Qt::InvertedPortraitOrientation);
    }
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if(event->key() == Qt::Key_Escape)
    {
         QCoreApplication::quit();
    }
    else
        QWidget::keyPressEvent(event);
}

bool isTouchEvent(const QEvent &event)
{
    if(event.type() == QEvent::TouchBegin)
        return true;
    if(event.type() == QEvent::TouchUpdate)
        return true;
    return false;    
}

bool isQuitCombination(const QTouchEvent &touchEvent)
{
    bool topLeftTouched = false;
    bool topRightTouched = false;
    bool bottomLeftTouched = false;
    bool bottomRightTouched = false;
    for(const auto &touchPoint : touchEvent.touchPoints())
    {
        const qreal normalizedCornerSize = 0.1;
        const qreal x = touchPoint.normalizedPos().x();
        const qreal y = touchPoint.normalizedPos().y();
        if(x < normalizedCornerSize)
        {
            if(y < normalizedCornerSize)
                topLeftTouched = true;
            else if(y > 1-normalizedCornerSize)
                bottomLeftTouched = true;
        }
        else if(x > 1-normalizedCornerSize)
        {
            if(y < normalizedCornerSize)
                topRightTouched = true;
            else if(y > 1-normalizedCornerSize)
                bottomRightTouched = true;
        }
    }
    return topLeftTouched && topRightTouched
        && bottomLeftTouched && bottomRightTouched;
}

bool MainWindow::event(QEvent* event)
{
    if(isTouchEvent(*event))
    {
        if(isQuitCombination(dynamic_cast<QTouchEvent&>(*event)))
            QCoreApplication::quit();
    }
    else
    {
        return QMainWindow::event(event);
    }
    return true;
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
   QMainWindow::resizeEvent(event);
   this->findChild<QLabel*>("image")->clear();
   updateImage();
}

void MainWindow::checkWindowSize()
{
  QScreen *screen = QGuiApplication::primaryScreen();
  if (screen != nullptr)
  {
    QSize screenSize = screen->geometry().size();
    if(size() != screenSize)
    {
      Log("Resizing Window ", screenSize.width(), "," , screenSize.height() );
      setFixedSize(screenSize);
      updateImage();
    }

    if (imageAspectMatchesMonitor)
    {
      bool isLandscape = screenSize.width() > screenSize.height();
      ImageAspectScreenFilter newAspect = isLandscape ? ImageAspectScreenFilter_Landscape : ImageAspectScreenFilter_Portrait;
      if (newAspect != baseImageOptions.onlyAspect)
      {
        Log("Changing image orientation to ", newAspect);
        baseImageOptions.onlyAspect = newAspect;
        currentImage.filename = "";
        warn("Monitor aspect changed, updating image...");
        repaint(); // force an immediate redraw as we might block for a while loading the next image
        if (switcher != nullptr)
        {
          // pick a new image as our aspect changed, we can't just resize the image
          switcher->scheduleImageUpdate();
        }
      }
    }
  }
  // make sure we are still fullscreen
  bool isFullScreen = windowState().testFlag(Qt::WindowFullScreen);
  if (!isFullScreen)
  {
    QTimer::singleShot(5, this, SLOT(showFullScreen()));
  }
}


void MainWindow::setImage(const ImageDetails &imageDetails)
{
    currentImage = imageDetails;
    updateImage();
}

void MainWindow::updateImage()
{
    checkWindowSize();
    if (currentImage.filename == "")
      return;

    QLabel *label = this->findChild<QLabel*>("image");
    QPixmap oldImage = label->pixmap(Qt::ReturnByValue);
    if (!oldImage.isNull() && transitionSeconds > 0)
    {
      QPalette palette;
      palette.setBrush(QPalette::Window, oldImage);
      this->setPalette(palette);
    }

    QPixmap p;
    p.load( currentImage.filename.c_str() );

    Log("size:", p.width(), "x", p.height(), "(window:", width(), ",", height(), ")");

    QPixmap rotated = getRotatedPixmap(p);
    QPixmap scaled = getScaledPixmap(rotated);
    QPixmap background = getBlurredBackground(rotated, scaled);
    drawForeground(background, scaled);
    
    if (overlay != nullptr)
    {
      drawText(background, overlay->getMarginTopLeft(), overlay->getFontsizeTopLeft(), overlay->getRenderedTopLeft(currentImage.filename).c_str(), Qt::AlignTop|Qt::AlignLeft);
      drawText(background, overlay->getMarginTopRight(), overlay->getFontsizeTopRight(), overlay->getRenderedTopRight(currentImage.filename).c_str(), Qt::AlignTop|Qt::AlignRight);
      drawText(background, overlay->getMarginBottomLeft(), overlay->getFontsizeBottomLeft(), overlay->getRenderedBottomLeft(currentImage.filename).c_str(), Qt::AlignBottom|Qt::AlignLeft);
      drawText(background, overlay->getMarginBottomRight(), overlay->getFontsizeBottomRight(), overlay->getRenderedBottomRight(currentImage.filename).c_str(), Qt::AlignBottom|Qt::AlignRight);
      /*if (ShouldLog())
      {
        // draw a thumbnail version of the source image in the bottom left, to check for cropping issues
        QPainter pt(&background);
        QBrush brush(QColor(255, 255, 255, 255));
        int margin = 10;
        QPixmap thumbNail = p.scaledToWidth(200, Qt::SmoothTransformation);
        pt.fillRect(background.width() - thumbNail.width() - 2*margin,
                     background.height()-thumbNail.height() - 2*margin,
                     thumbNail.width() +2*margin, thumbNail.height()+2*margin, brush);

        pt.drawPixmap( background.width() - thumbNail.width() - margin,
                     background.height()-thumbNail.height() - margin, 
                     thumbNail);
      }*/
    }

    label->setPixmap(background);

    if (!oldImage.isNull() && transitionSeconds > 0)
    {
      auto effect = new QGraphicsOpacityEffect(label);
      effect->setOpacity(0.0);
      label->setGraphicsEffect(effect);
      QPropertyAnimation* animation = new QPropertyAnimation(effect, "opacity");
      animation->setDuration(transitionSeconds*1000);
      animation->setStartValue(0);
      animation->setEndValue(1);
      animation->start(QAbstractAnimation::DeleteWhenStopped);
    }

    update();
}

void MainWindow::drawText(QPixmap& image, int margin, int fontsize, QString text, int alignment) {
  QPainter pt(&image);
  pt.setPen(QPen(QColor(overlayHexRGB)));
  pt.setFont(QFont("Sans", fontsize, QFont::Bold));
  QRect marginRect = image.rect().adjusted(
      margin,
      margin,
      margin*-1,
      margin*-1);
  pt.drawText(marginRect, alignment, text);
}

void MainWindow::drawForeground(QPixmap& background, const QPixmap& foreground) {
    QPainter pt(&background);
    QBrush brush(QColor(0, 0, 0, 255-backgroundOpacity));
    pt.fillRect(0,0,background.width(), background.height(), brush);
    pt.drawPixmap((background.width()-foreground.width())/2, (background.height()-foreground.height())/2, foreground);
}

void MainWindow::setOverlay(std::unique_ptr<Overlay> &o)
{
  overlay = std::move(o);
}

QPixmap MainWindow::getBlurredBackground(const QPixmap& originalSize, const QPixmap& scaled)
{
    if (currentImage.options.fitAspectAxisToWindow) {
      // our scaled version will just fill the whole screen, use it directly
      //Log("Using scaled image");
      QRect rect((scaled.width() - width())/2, 0, width(), height());
      return scaled.copy(rect);
    } else if (scaled.width() < width()) {
      QPixmap background = blur(originalSize.scaledToWidth(width(), Qt::SmoothTransformation));
      QRect rect(0, (background.height() - height())/2, width(), height());
      return background.copy(rect);
    } else {
      // aspect 'p' or the image is not as wide as the screen
      QPixmap background = blur(originalSize.scaledToHeight(height(), Qt::SmoothTransformation));
      QRect rect((background.width() - width())/2, 0, width(), height());
      return background.copy(rect);
    }
}

QPixmap MainWindow::getRotatedPixmap(const QPixmap& p)
{
    QTransform transform;
    transform.rotate(currentImage.rotation);
    return p.transformed(transform);
}

QPixmap MainWindow::getScaledPixmap(const QPixmap& p)
{
  if (currentImage.options.fitAspectAxisToWindow)
  {
    bool stretchWidth = currentImage.aspect() == ImageAspect_Landscape;
    bool stretchHeight = currentImage.aspect() == ImageAspect_Portrait;
    // check the stretched image will naturally fill the screen for its aspect ratio
    if (stretchHeight && (width() > ((double)height()/p.height())*p.width()))
    {
      // stretched via height won't fill the width, so stretch the other way
      stretchHeight = false;
      stretchWidth = true;
    }
    else if (stretchWidth && (height() > ((double)width()/p.width())*p.height()))
    {
      // stretched via width won't fill the width, so stretch the other way
      stretchWidth = false;
      stretchHeight = true;
    }

    if (stretchHeight)
    {
      // potrait mode, make height of image fit screen and crop top/bottom
      QPixmap pTemp = p.scaledToHeight(height(), Qt::SmoothTransformation);
      return pTemp.copy(0,0,width(),height());
    }
    else if (stretchWidth)
    {
      // landscape mode, make width of image fit screen and crop top/bottom
      QPixmap pTemp = p.scaledToWidth(width(), Qt::SmoothTransformation);
      return pTemp.copy(0,0,width(),height());
    }
  }

  // just scale the best we can for the given photo
  int w = width();
  int h = height();
  return p.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

void MainWindow::drawBackground(const QPixmap& originalSize, const QPixmap& scaled)
{
    QPalette palette;
    if (scaled.width() < width()) {
        QPixmap background = blur(originalSize.scaledToHeight(height()));
        QRect rect((background.width() - width())/2, 0, width(), height());
        background = background.copy(rect);
        palette.setBrush(QPalette::Window, background);
    } else {
        QPixmap background = blur(originalSize.scaledToHeight(height()));
        QRect rect((background.width() - width())/2, 0, width(), height());
        background = background.copy(rect);
        palette.setBrush(QPalette::Window, background);
    }
    this->setPalette(palette);
}

QPixmap MainWindow::blur(const QPixmap& input)
{
    QGraphicsScene scene;
    QGraphicsPixmapItem item;
    item.setPixmap(input);
    QGraphicsBlurEffect effect;
    effect.setBlurRadius(blurRadius);
    item.setGraphicsEffect(&effect);
    scene.addItem(&item);
    QImage res(input.size(), QImage::Format_ARGB32);
    res.fill(Qt::transparent);
    QPainter ptr(&res);
    scene.render(&ptr, QRectF(), QRectF( 0, 0, input.width(), input.height()) );
    return QPixmap::fromImage(res);
}

void MainWindow::setBlurRadius(unsigned int blurRadius)
{
    this->blurRadius = blurRadius;
}

void MainWindow::setBackgroundOpacity(unsigned int backgroundOpacity)
{
    this->backgroundOpacity = backgroundOpacity;
}

void MainWindow::setOverlayHexRGB(QString overlayHexRGB)
{
    this->overlayHexRGB = overlayHexRGB;
}

void MainWindow::setTransitionTime(unsigned int transitionSeconds)
{
    this->transitionSeconds = transitionSeconds;
}

void MainWindow::warn(std::string text)
{
  QLabel *label = this->findChild<QLabel*>("image");
  label->setText(text.c_str());
}

void MainWindow::setBaseOptions(const ImageDisplayOptions &baseOptionsIn) 
{ 
  baseImageOptions = baseOptionsIn; 
  imageAspectMatchesMonitor = false;
  if(baseImageOptions.onlyAspect == ImageAspectScreenFilter_Monitor)
  {
    imageAspectMatchesMonitor = true;
    baseImageOptions.onlyAspect = width() >= height() ? ImageAspectScreenFilter_Landscape : ImageAspectScreenFilter_Portrait;
    switcher->scheduleImageUpdate();
  }
}

void MainWindow::setImageSwitcher(ImageSwitcher *switcherIn)
{
   switcher = switcherIn; 
}

const ImageDisplayOptions &MainWindow::getBaseOptions() 
{
   return baseImageOptions; 
}

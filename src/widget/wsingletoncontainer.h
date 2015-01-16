// WSingletonContainer defines widgets that should only be instantiated once
// but may appear in multiple places in a skin definition.  This is useful
// for complex widgets like the library, which are memory intensive. The
// container mostly looks like a special WidgetGroup which is defined in
// special ways.
//
// Usage:
// First, the Singleton container is defined, meaning it is described to the
// skin system by name, and what the singleton consists of.  This definition
// should be very early in the skin file.  Note that the singleton does not
// actually appear where it is defined.
//
// Example definition:
// <SingletonDefinition>
//   <ObjectName>LibrarySingleton</ObjectName>
//   <Layout>horizontal</Layout>
//   <SizePolicy>me,me</SizePolicy>
//   <Children>
//     <Template src="skin:library.xml"/>
//   </Children>
// </SingletonDefinition>
//
// The ObjectName is used to identify this singleton elsewhere in the skin
// files.
//
// Example usage:
// <WidgetGroup>
//    <ObjectName>SomeUiElement</ObjectName>
//    <Layout>vertical</Layout>
//    <SizePolicy>min,i</SizePolicy>
//    <Children>
//      <SingletonContainer objectName="LibrarySingleton"/>
//      ...
//    </Children>
// </WidgetGroup>
//
// The skin system sees the Singleton tag, and any time the containing
// group gets a show event, the Singleton widget is reparented to this location
// in the skin.  Note that if a Singleton is visible twice at the same time,
// behavior is undefined and could be crashy.

#ifndef WSINGLETONCONTAINER_H
#define WSINGLETONCONTAINER_H

#include <QPointer>

#include "widget/wwidgetgroup.h"

class WSingletonContainer : public WWidgetGroup {
    Q_OBJECT
  public:
    // Prepares the container and remembers the widget, but does not add the
    // widget to the container.
    WSingletonContainer(QWidget* widget, QWidget* pParent=NULL);

  public slots:
    virtual void showEvent(QShowEvent* event);

  private:
    QPointer<QWidget> m_pWidget;
    QLayout* m_pLayout;
};

class SingletonMap {
  public:
    typedef QMap<QString, QWidget*> WidgetMap;

    // Takes a constructed QWidget and inserts it in the map of available
    // singletons.  Checks that an object of that name hasn't already been
    // defined.
    void defineSingleton(QString objectName, QWidget* widget);

    // We don't want to end up with badly-constructed containers, so only
    // provide a factory function.  Returns NULL if the objectName is not in
    // the map.
    WSingletonContainer* getSingleton(QString objectName,
                                      QWidget* pParent=NULL);

  private:
    WidgetMap m_singletons;
};


#endif  // WSINGLETONCONTAINER_H

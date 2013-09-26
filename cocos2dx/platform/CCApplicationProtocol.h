#ifndef __CC_APPLICATION_PROTOCOL_H__
#define __CC_APPLICATION_PROTOCOL_H__

NS_CC_BEGIN

/**
 * @addtogroup platform
 * @{
 */

class CC_DLL ApplicationProtocol
{
public:

    // Since WINDOWS and ANDROID are defined as macros, we could not just use these keywords in enumeration(Platform).
    // Therefore, 'OS_' prefix is added to avoid conflicts with the definitions of system macros.
    enum class Platform
    {
        OS_WINDOWS,
        OS_LINUX,
        OS_MAC,
        OS_ANDROID,
        OS_IPHONE,
        OS_IPAD,
        OS_BLACKBERRY,
        OS_NACL,
        OS_EMSCRIPTEN,
        OS_TIZEN
    };

    /**
     * @js NA
     * @lua NA
     */
    virtual ~ApplicationProtocol() {}

    /**
    @brief    Implement Director and Scene init code here.
    @return true    Initialize success, app continue.
    @return false   Initialize failed, app terminate.
    * @js NA
    * @lua NA
    */
    virtual bool applicationDidFinishLaunching() = 0;

    /**
    @brief  This function will be called when the application enters background.
    * @js NA
    * @lua NA
    */
    virtual void applicationDidEnterBackground() = 0;

    /**
    @brief  This function will be called when the application enters foreground.
    * @js NA
    * @lua NA
    */
    virtual void applicationWillEnterForeground() = 0;

    /**
     @brief  This function will be called when the application enters background.
     * @js NA
     * @lua NA
     */
    virtual void applicationWillResignActive() = 0;
    
    /**
     @brief  This function will be called when the application enters foreground.
     * @js NA
     * @lua NA
     */
    virtual void applicationDidBecomeActive() = 0;

    /**
    @brief    Callback by Director for limit FPS.
    @param interval The time, expressed in seconds, between current frame and next.
    * @js NA
    * @lua NA
    */
    virtual void setAnimationInterval(double interval) = 0;

    /**
    @brief Get current language config
    @return Current language config
    * @js NA
    * @lua NA
    */
    virtual LanguageType getCurrentLanguage() = 0;
    
    /**
     @brief Get target platform
     * @js NA
     * @lua NA
     */
    virtual Platform getTargetPlatform() = 0;
};

// end of platform group
/// @}

NS_CC_END

#endif    // __CC_APPLICATION_PROTOCOL_H__

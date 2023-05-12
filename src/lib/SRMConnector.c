#include <private/SRMConnectorPrivate.h>
#include <private/SRMDevicePrivate.h>
#include <private/SRMEncoderPrivate.h>
#include <private/SRMCrtcPrivate.h>
#include <private/SRMPlanePrivate.h>
#include <SRMLog.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <unistd.h>


SRMDevice *srmConnectorGetDevice(SRMConnector *connector)
{
    return connector->device;
}

UInt32 srmConnectorGetID(SRMConnector *connector)
{
    return connector->id;
}

SRM_CONNECTOR_STATE srmConnectorGetState(SRMConnector *connector)
{
    return connector->state;
}

UInt8 srmConnectorIsConnected(SRMConnector *connector)
{
    return connector->connected;
}

UInt32 srmConnectorGetmmWidth(SRMConnector *connector)
{
    return connector->mmWidth;
}

UInt32 srmConnectorGetmmHeight(SRMConnector *connector)
{
    return connector->mmWidth;
}

UInt32 srmConnectorGetType(SRMConnector *connector)
{
    return connector->type;
}

const char *srmConnectorGetName(SRMConnector *connector)
{
    return connector->name ? connector->name : "Unknown";
}

const char *srmConnectorGetManufacturer(SRMConnector *connector)
{
    return connector->manufacturer ? connector->manufacturer : "Unknown";
}

const char *srmConnectorGetModel(SRMConnector *connector)
{
    return connector->model ? connector->model : "Unknown";
}

SRMList *srmConnectorGetEncoders(SRMConnector *connector)
{
    return connector->encoders;
}

SRMList *srmConnectorGetModes(SRMConnector *connector)
{
    return connector->modes;
}

UInt8 srmConnectorHasHardwareCursor(SRMConnector *connector)
{
    return connector->cursorBO != NULL;
}

UInt8 srmConnectorSetCursor(SRMConnector *connector, UInt8 *pixels)
{
    if (!connector->cursorBO)
        return 0;

    gbm_bo_write(connector->cursorBO, pixels, 64*64*4);

    drmModeSetCursor(connector->device->fd,
                     connector->currentCrtc->id,
                     gbm_bo_get_handle(connector->cursorBO).u32,
                     64,
                     64);

    return 1;
}

UInt8 srmConnectorSetCursorPos(SRMConnector *connector, Int32 x, Int32 y)
{
    if (!connector->cursorBO)
        return 0;

    drmModeMoveCursor(connector->device->fd,
                      connector->currentCrtc->id,
                      x,
                      y);

    return 1;
}

SRMEncoder *srmConnectorGetCurrentEncoder(SRMConnector *connector)
{
    return connector->currentEncoder;
}

SRMCrtc *srmConnectorGetCurrentCrtc(SRMConnector *connector)
{
    return connector->currentCrtc;
}

SRMPlane *srmConnectorGetCurrentPrimaryPlane(SRMConnector *connector)
{
    return connector->currentPrimaryPlane;
}

SRMPlane *srmConnectorGetCurrentCursorPlane(SRMConnector *connector)
{
    return connector->currentCursorPlane;
}

SRMConnectorMode *srmConnectorGetPreferredMode(SRMConnector *connector)
{
    return connector->preferredMode;
}

SRMConnectorMode *srmConnectorGetCurrentMode(SRMConnector *connector)
{
    return connector->currentMode;
}

UInt8 srmConnectorSetMode(SRMConnector *connector, SRMConnectorMode *mode)
{
    if (connector->state == SRM_CONNECTOR_STATE_INITIALIZED)
    {
        if (connector->currentMode == mode)
            return 1;

        SRMConnectorMode *modeBackup = connector->currentMode;

        connector->targetMode = mode;
        connector->state = SRM_CONNECTOR_STATE_CHANGING_MODE;
        srmConnectorUnlockRenderThread(connector);

        while (connector->state == SRM_CONNECTOR_STATE_CHANGING_MODE)
        {
            usleep(200);
        }

        if (connector->state == SRM_CONNECTOR_STATE_INITIALIZED)
        {
            return 1;
        }
        else // REVERTING MODE CHANGE
        {
            connector->targetMode = modeBackup;
            connector->state = SRM_CONNECTOR_STATE_CHANGING_MODE;
            srmConnectorUnlockRenderThread(connector);
            while (connector->state == SRM_CONNECTOR_STATE_CHANGING_MODE)
            {
                usleep(200);
            }
            return 0;
        }
    }

    else if (connector->state == SRM_CONNECTOR_STATE_UNINITIALIZED)
    {
        connector->currentMode = mode;
        return 1;
    }

    // Wait for intermediate states to finish
    else if (connector->state == SRM_CONNECTOR_STATE_INITIALIZING ||
        connector->state == SRM_CONNECTOR_STATE_UNINITIALIZING ||
        connector->state == SRM_CONNECTOR_STATE_CHANGING_MODE)
    {
        while (connector->state == SRM_CONNECTOR_STATE_UNINITIALIZING ||
               connector->state == SRM_CONNECTOR_STATE_UNINITIALIZING ||
               connector->state == SRM_CONNECTOR_STATE_CHANGING_MODE)
        {
            usleep(200);
        }

        return srmConnectorSetMode(connector, mode);
    }

    return 1;
}

UInt8 srmConnectorInitialize(SRMConnector *connector, SRMConnectorInterface *interface, void *userData)
{
    if (connector->state != SRM_CONNECTOR_STATE_UNINITIALIZED)
        return 0;

    connector->state = SRM_CONNECTOR_STATE_INITIALIZING;

    // Find best config
    SRMEncoder *bestEncoder;
    SRMCrtc *bestCrtc;
    SRMPlane *bestPrimaryPlane;
    SRMPlane *bestCursorPlane;

    if (!srmConnectorGetBestConfiguration(connector, &bestEncoder, &bestCrtc, &bestPrimaryPlane, &bestCursorPlane))
    {
        // Fails to get a valid encoder, crtc or primary plane
        SRMWarning("Could not get a Encoder, Crtc and Primary Plane trio for device %s connector %d.",
                   connector->device->name,
                   connector->id);
        return 0;
    }

    connector->currentEncoder = bestEncoder;
    connector->currentCrtc = bestCrtc;
    connector->currentPrimaryPlane = bestPrimaryPlane;
    connector->currentCursorPlane = bestCursorPlane;

    bestEncoder->currentConnector = connector;
    bestCrtc->currentConnector = connector;
    bestPrimaryPlane->currentConnector = connector;

    if (bestCursorPlane)
        bestCursorPlane->currentConnector = connector;

    connector->interfaceData = userData;
    connector->interface = interface;

    connector->renderInitResult = 0;

    if (pthread_create(&connector->renderThread, NULL, srmConnectorRenderThread, connector))
    {
        SRMError("Could not start render thread for device %s connector %d.", connector->device->name, connector->id);
        goto fail;
    }

    while (!connector->renderInitResult) { usleep(20000); }

    // If failed
    if (connector->renderInitResult != 1)
        goto fail;

    connector->state = SRM_CONNECTOR_STATE_INITIALIZED;

    SRMDebug("[%s] Connector (%d) %s, %s, %s initialized.",
             connector->device->name,
             connector->id,
             connector->name,
             connector->model,
             connector->manufacturer);

    return 1;

fail:
    connector->renderThread = 0;
    connector->currentEncoder = NULL;
    connector->currentCrtc = NULL;
    connector->currentPrimaryPlane = NULL;
    connector->currentCursorPlane = NULL;

    bestEncoder->currentConnector = NULL;
    bestCrtc->currentConnector = NULL;
    bestPrimaryPlane->currentConnector = NULL;

    if (bestCursorPlane)
        bestCursorPlane->currentConnector = NULL;

    connector->interfaceData = NULL;
    connector->interface = NULL;

    connector->state = SRM_CONNECTOR_STATE_UNINITIALIZED;
    return 0;
}

UInt8 srmConnectorRepaint(SRMConnector *connector)
{
    if (connector->state == SRM_CONNECTOR_STATE_INITIALIZING ||
        connector->state == SRM_CONNECTOR_STATE_INITIALIZED ||
        connector->state == SRM_CONNECTOR_STATE_CHANGING_MODE)
    {

        if (!connector->repaintRequested)
            srmConnectorUnlockRenderThread(connector);

        return 1;
    }

    return 0;
}

void srmConnectorUninitialize(SRMConnector *connector)
{
    // Wait for those states to change
    while (connector->state == SRM_CONNECTOR_STATE_CHANGING_MODE ||
           connector->state == SRM_CONNECTOR_STATE_INITIALIZING)
    {
        usleep(20000);
    }


    // Nothing to do
    if (connector->state == SRM_CONNECTOR_STATE_UNINITIALIZED ||
        connector->state == SRM_CONNECTOR_STATE_UNINITIALIZING)
    {
        return;
    }

    // Unitialize
    connector->state = SRM_CONNECTOR_STATE_UNINITIALIZING;
    srmConnectorUnlockRenderThread(connector);

    while (connector->state != SRM_CONNECTOR_STATE_UNINITIALIZED)
        usleep(20000);

    if (connector->currentCrtc)
    {
        connector->currentCrtc->currentConnector = NULL;
        connector->currentCrtc = NULL;
    }

    if (connector->currentEncoder)
    {
        connector->currentEncoder->currentConnector = NULL;
        connector->currentEncoder = NULL;
    }

    if (connector->currentPrimaryPlane)
    {
        connector->currentPrimaryPlane->currentConnector = NULL;
        connector->currentPrimaryPlane = NULL;
    }

    if (connector->currentCursorPlane)
    {
        /* TODO: Set the cursor to a connector that needs it */
        connector->currentCursorPlane->currentConnector = NULL;
        connector->currentCursorPlane = NULL;
    }

    if (connector->cursorBO)
    {
        gbm_bo_destroy(connector->cursorBO);
        connector->cursorBO = NULL;
    }

    pthread_mutex_destroy(&connector->repaintMutex);
    pthread_cond_destroy(&connector->repaintCond);

    connector->interfaceData = NULL;
    connector->interface = NULL;

    SRMDebug("[%s] Connector (%d) %s, %s, %s uninitialized.",
             connector->device->name,
             connector->id,
             connector->name,
             connector->model,
             connector->manufacturer);

    /* TODO */

}

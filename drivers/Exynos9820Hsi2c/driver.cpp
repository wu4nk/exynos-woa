#include "internal.h"
#include "driver.h"
#include "device.h"

#include "driver.tmh"

NTSTATUS
#pragma prefast(suppress:__WARNING_DRIVER_FUNCTION_TYPE, "DriverEntry uses the WDF entry contract")
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG driverConfig;
    WDF_OBJECT_ATTRIBUTES driverAttributes;
    NTSTATUS status;

    WPP_INIT_TRACING(DriverObject, RegistryPath);

    WDF_DRIVER_CONFIG_INIT(&driverConfig, OnDeviceAdd);
    driverConfig.DriverPoolTag = EHI2C_POOL_TAG;

    WDF_OBJECT_ATTRIBUTES_INIT(&driverAttributes);
    driverAttributes.EvtCleanupCallback = OnDriverCleanup;

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &driverAttributes,
        &driverConfig,
        WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
    {
        WPP_CLEANUP(DriverObject);
    }

    return status;
}

VOID
OnDriverCleanup(
    _In_ WDFOBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
    WPP_CLEANUP(NULL);
}

NTSTATUS
OnDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFDEVICE fxDevice;
    PPBC_DEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    status = SpbDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, PBC_DEVICE);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &fxDevice);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    device = GetDeviceContext(fxDevice);
    device->FxDevice = fxDevice;

    {
        SPB_CONTROLLER_CONFIG spbConfig;

        SPB_CONTROLLER_CONFIG_INIT(&spbConfig);
        spbConfig.EvtSpbTargetConnect = OnTargetConnect;
        spbConfig.ControllerDispatchType = WdfIoQueueDispatchSequential;
        spbConfig.PowerManaged = WdfTrue;
        spbConfig.EvtSpbIoRead = OnRead;
        spbConfig.EvtSpbIoWrite = OnWrite;
        spbConfig.EvtSpbIoSequence = OnSequence;
        spbConfig.EvtSpbControllerLock = OnControllerLock;
        spbConfig.EvtSpbControllerUnlock = OnControllerUnlock;

        status = SpbDeviceInitialize(fxDevice, &spbConfig);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    {
        WDF_OBJECT_ATTRIBUTES targetAttributes;
        WDF_OBJECT_ATTRIBUTES requestAttributes;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&targetAttributes, PBC_TARGET);
        SpbControllerSetTargetAttributes(fxDevice, &targetAttributes);

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestAttributes, PBC_REQUEST);
        SpbControllerSetRequestAttributes(fxDevice, &requestAttributes);
    }

    {
        WDF_INTERRUPT_CONFIG interruptConfig;

        WDF_INTERRUPT_CONFIG_INIT(
            &interruptConfig,
            OnInterruptIsr,
            OnInterruptDpc);
        interruptConfig.AutomaticSerialization = FALSE;

        status = WdfInterruptCreate(
            fxDevice,
            &interruptConfig,
            WDF_NO_OBJECT_ATTRIBUTES,
            &device->InterruptObject);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    {
        WDF_OBJECT_ATTRIBUTES lockAttributes;

        WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
        lockAttributes.ParentObject = fxDevice;
        status = WdfSpinLockCreate(&lockAttributes, &device->Lock);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttributes;

        WDF_TIMER_CONFIG_INIT(&timerConfig, OnDelayTimerExpired);
        timerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
        timerAttributes.ParentObject = fxDevice;
        timerAttributes.ExecutionLevel = WdfExecutionLevelDispatch;
        timerAttributes.SynchronizationScope = WdfSynchronizationScopeNone;

        status = WdfTimerCreate(
            &timerConfig,
            &timerAttributes,
            &device->DelayTimer);
        if (!NT_SUCCESS(status))
        {
            return status;
        }

        WDF_TIMER_CONFIG_INIT(&timerConfig, OnRequestTimeout);
        timerConfig.AutomaticSerialization = FALSE;
        status = WdfTimerCreate(
            &timerConfig,
            &timerAttributes,
            &device->RequestTimer);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    {
        WDF_WORKITEM_CONFIG workItemConfig;
        WDF_OBJECT_ATTRIBUTES workItemAttributes;

        WDF_WORKITEM_CONFIG_INIT(&workItemConfig, OnTelemetryWorkItem);
        WDF_OBJECT_ATTRIBUTES_INIT(&workItemAttributes);
        workItemAttributes.ParentObject = fxDevice;

        status = WdfWorkItemCreate(
            &workItemConfig,
            &workItemAttributes,
            &device->TelemetryWorkItem);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    {
        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idleSettings;

        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(
            &idleSettings,
            IdleCannotWakeFromS0);
        idleSettings.IdleTimeoutType = SystemManagedIdleTimeoutWithHint;
        idleSettings.IdleTimeout = 1000;
        //
        // S10 (2026-11-03): stay in D0. This bus carries only the touch controller, which is read up to
        // ~120 times a second; idling to D3 after 1 s made the first transfer of every touch wait for a
        // D0 transition, and a touch client opening its SPB target while the controller sat in D3 got
        // STATUS_INVALID_DEVICE_STATE (S10Touch PRAM: "SpbOpen c0000184") - no touch at all.
        //
        idleSettings.Enabled = WdfFalse;

        status = WdfDeviceAssignS0IdleSettings(fxDevice, &idleSettings);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    return STATUS_SUCCESS;
}

/*
 * Copyright IBM Corp. 2008
 *
 * lxc_driver.c: linux container driver functions
 *
 * Authors:
 *  David L. Leskovec <dlesko at linux.vnet.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <config.h>

#include <fcntl.h>
#include <sched.h>
#include <sys/utsname.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <unistd.h>
#include <wait.h>

#include "virterror_internal.h"
#include "logging.h"
#include "datatypes.h"
#include "lxc_conf.h"
#include "lxc_container.h"
#include "lxc_driver.h"
#include "memory.h"
#include "util.h"
#include "bridge.h"
#include "veth.h"
#include "event.h"
#include "nodeinfo.h"
#include "uuid.h"


#define VIR_FROM_THIS VIR_FROM_LXC

static int lxcStartup(int privileged);
static int lxcShutdown(void);
static lxc_driver_t *lxc_driver = NULL;

/* Functions */

static void lxcDriverLock(lxc_driver_t *driver)
{
    virMutexLock(&driver->lock);
}
static void lxcDriverUnlock(lxc_driver_t *driver)
{
    virMutexUnlock(&driver->lock);
}

static void lxcDomainEventFlush(int timer, void *opaque);
static void lxcDomainEventQueue(lxc_driver_t *driver,
                                virDomainEventPtr event);


static virDrvOpenStatus lxcOpen(virConnectPtr conn,
                                virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                                int flags ATTRIBUTE_UNUSED)
{
    /* Verify uri was specified */
    if (conn->uri == NULL) {
        if (lxc_driver == NULL)
            return VIR_DRV_OPEN_DECLINED;

        conn->uri = xmlParseURI("lxc:///");
        if (!conn->uri) {
            virReportOOMError(conn);
            return VIR_DRV_OPEN_ERROR;
        }
    } else {
        if (conn->uri->scheme == NULL ||
            STRNEQ(conn->uri->scheme, "lxc"))
            return VIR_DRV_OPEN_DECLINED;

        /* Leave for remote driver */
        if (conn->uri->server != NULL)
            return VIR_DRV_OPEN_DECLINED;

        /* If path isn't '/' then they typoed, tell them correct path */
        if (STRNEQ(conn->uri->path, "/")) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("unexpected LXC URI path '%s', try lxc:///"),
                     conn->uri->path);
            return VIR_DRV_OPEN_ERROR;
        }

        /* URI was good, but driver isn't active */
        if (lxc_driver == NULL) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     "%s", _("lxc state driver is not active"));
            return VIR_DRV_OPEN_ERROR;
        }
    }

    conn->privateData = lxc_driver;

    return VIR_DRV_OPEN_SUCCESS;
}

static int lxcClose(virConnectPtr conn)
{
    lxc_driver_t *driver = conn->privateData;

    lxcDriverLock(driver);
    virDomainEventCallbackListRemoveConn(conn, driver->domainEventCallbacks);
    lxcDriverUnlock(driver);

    conn->privateData = NULL;
    return 0;
}

static char *lxcGetCapabilities(virConnectPtr conn) {
    lxc_driver_t *driver = conn->privateData;
    char *xml;

    lxcDriverLock(driver);
    if ((xml = virCapabilitiesFormatXML(driver->caps)) == NULL)
        virReportOOMError(conn);
    lxcDriverUnlock(driver);

    return xml;
}


static virDomainPtr lxcDomainLookupByID(virConnectPtr conn,
                                        int id)
{
    lxc_driver_t *driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    lxcDriverLock(driver);
    vm = virDomainFindByID(&driver->domains, id);
    lxcDriverUnlock(driver);

    if (!vm) {
        lxcError(conn, NULL, VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}

static virDomainPtr lxcDomainLookupByUUID(virConnectPtr conn,
                                          const unsigned char *uuid)
{
    lxc_driver_t *driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        lxcError(conn, NULL, VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}

static virDomainPtr lxcDomainLookupByName(virConnectPtr conn,
                                          const char *name)
{
    lxc_driver_t *driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    lxcDriverLock(driver);
    vm = virDomainFindByName(&driver->domains, name);
    lxcDriverUnlock(driver);
    if (!vm) {
        lxcError(conn, NULL, VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}

static int lxcListDomains(virConnectPtr conn, int *ids, int nids) {
    lxc_driver_t *driver = conn->privateData;
    int n;

    lxcDriverLock(driver);
    n = virDomainObjListGetActiveIDs(&driver->domains, ids, nids);
    lxcDriverUnlock(driver);

    return n;
}

static int lxcNumDomains(virConnectPtr conn) {
    lxc_driver_t *driver = conn->privateData;
    int n;

    lxcDriverLock(driver);
    n = virDomainObjListNumOfDomains(&driver->domains, 1);
    lxcDriverUnlock(driver);

    return n;
}

static int lxcListDefinedDomains(virConnectPtr conn,
                                 char **const names, int nnames) {
    lxc_driver_t *driver = conn->privateData;
    int n;

    lxcDriverLock(driver);
    n = virDomainObjListGetInactiveNames(&driver->domains, names, nnames);
    lxcDriverUnlock(driver);

    return n;
}


static int lxcNumDefinedDomains(virConnectPtr conn) {
    lxc_driver_t *driver = conn->privateData;
    int n;

    lxcDriverLock(driver);
    n = virDomainObjListNumOfDomains(&driver->domains, 0);
    lxcDriverUnlock(driver);

    return n;
}



static virDomainPtr lxcDomainDefine(virConnectPtr conn, const char *xml)
{
    lxc_driver_t *driver = conn->privateData;
    virDomainDefPtr def = NULL;
    virDomainObjPtr vm = NULL;
    virDomainPtr dom = NULL;
    virDomainEventPtr event = NULL;
    int newVM = 1;

    lxcDriverLock(driver);
    if (!(def = virDomainDefParseString(conn, driver->caps, xml,
                                        VIR_DOMAIN_XML_INACTIVE)))
        goto cleanup;

    /* See if a VM with matching UUID already exists */
    vm = virDomainFindByUUID(&driver->domains, def->uuid);
    if (vm) {
        /* UUID matches, but if names don't match, refuse it */
        if (STRNEQ(vm->def->name, def->name)) {
            char uuidstr[VIR_UUID_STRING_BUFLEN];
            virUUIDFormat(vm->def->uuid, uuidstr);
            lxcError(conn, NULL, VIR_ERR_OPERATION_FAILED,
                     _("domain '%s' is already defined with uuid %s"),
                     vm->def->name, uuidstr);
            goto cleanup;
        }

        /* UUID & name match */
        virDomainObjUnlock(vm);
        newVM = 0;
    } else {
        /* UUID does not match, but if a name matches, refuse it */
        vm = virDomainFindByName(&driver->domains, def->name);
        if (vm) {
            char uuidstr[VIR_UUID_STRING_BUFLEN];
            virUUIDFormat(vm->def->uuid, uuidstr);
            lxcError(conn, NULL, VIR_ERR_OPERATION_FAILED,
                     _("domain '%s' is already defined with uuid %s"),
                     def->name, uuidstr);
            goto cleanup;
        }
    }

    if ((def->nets != NULL) && !(driver->have_netns)) {
        lxcError(conn, NULL, VIR_ERR_NO_SUPPORT,
                 "%s", _("System lacks NETNS support"));
        goto cleanup;
    }

    if (!(vm = virDomainAssignDef(conn, driver->caps,
                                  &driver->domains, def)))
        goto cleanup;
    def = NULL;
    vm->persistent = 1;

    if (virDomainSaveConfig(conn,
                            driver->configDir,
                            vm->newDef ? vm->newDef : vm->def) < 0) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
        goto cleanup;
    }

    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_DEFINED,
                                     newVM ?
                                     VIR_DOMAIN_EVENT_DEFINED_ADDED :
                                     VIR_DOMAIN_EVENT_DEFINED_UPDATED);

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    virDomainDefFree(def);
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        lxcDomainEventQueue(driver, event);
    lxcDriverUnlock(driver);
    return dom;
}

static int lxcDomainUndefine(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 "%s", _("no domain with matching uuid"));
        goto cleanup;
    }

    if (virDomainObjIsActive(vm)) {
        lxcError(dom->conn, dom, VIR_ERR_OPERATION_INVALID,
                 "%s", _("cannot delete active domain"));
        goto cleanup;
    }

    if (!vm->persistent) {
        lxcError(dom->conn, dom, VIR_ERR_OPERATION_INVALID,
                 "%s", _("cannot undefine transient domain"));
        goto cleanup;
    }

    if (virDomainDeleteConfig(dom->conn,
                              driver->configDir,
                              driver->autostartDir,
                              vm) < 0)
        goto cleanup;

    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_UNDEFINED,
                                     VIR_DOMAIN_EVENT_UNDEFINED_REMOVED);

    virDomainRemoveInactive(&driver->domains, vm);
    vm = NULL;
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        lxcDomainEventQueue(driver, event);
    lxcDriverUnlock(driver);
    return ret;
}

static int lxcDomainGetInfo(virDomainPtr dom,
                            virDomainInfoPtr info)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virCgroupPtr cgroup = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 "%s", _("no domain with matching uuid"));
        goto cleanup;
    }

    info->state = vm->state;

    if (!virDomainObjIsActive(vm) || driver->cgroup == NULL) {
        info->cpuTime = 0;
        info->memory = vm->def->memory;
    } else {
        if (virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) != 0) {
            lxcError(dom->conn, dom, VIR_ERR_INTERNAL_ERROR,
                     _("Unable to get cgroup for %s\n"), vm->def->name);
            goto cleanup;
        }

        if (virCgroupGetCpuacctUsage(cgroup, &(info->cpuTime)) < 0) {
            lxcError(dom->conn, dom, VIR_ERR_OPERATION_FAILED,
                     "%s", _("cannot read cputime for domain"));
            goto cleanup;
        }
        if (virCgroupGetMemoryUsage(cgroup, &(info->memory)) < 0) {
            lxcError(dom->conn, dom, VIR_ERR_OPERATION_FAILED,
                     "%s", _("cannot read memory usage for domain"));
            goto cleanup;
        }
    }

    info->maxMem = vm->def->maxmem;
    info->nrVirtCpu = 1;
    ret = 0;

cleanup:
    lxcDriverUnlock(driver);
    if (cgroup)
        virCgroupFree(&cgroup);
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static char *lxcGetOSType(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *ret = NULL;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 "%s", _("no domain with matching uuid"));
        goto cleanup;
    }

    ret = strdup(vm->def->os.type);

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

/* Returns max memory in kb, 0 if error */
static unsigned long lxcDomainGetMaxMemory(virDomainPtr dom) {
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    unsigned long ret = 0;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(dom->conn, dom, VIR_ERR_NO_DOMAIN,
                         _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    ret = vm->def->maxmem;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int lxcDomainSetMaxMemory(virDomainPtr dom, unsigned long newmax) {
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(dom->conn, dom, VIR_ERR_NO_DOMAIN,
                         _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (newmax < vm->def->memory) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_ARG,
                         "%s", _("cannot set max memory lower than current memory"));
        goto cleanup;
    }

    vm->def->maxmem = newmax;
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int lxcDomainSetMemory(virDomainPtr dom, unsigned long newmem) {
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virCgroupPtr cgroup = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);
    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(dom->conn, dom, VIR_ERR_NO_DOMAIN,
                 _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (newmem > vm->def->maxmem) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_ARG,
                 "%s", _("cannot set memory higher than max memory"));
        goto cleanup;
    }

    if (virDomainObjIsActive(vm)) {
        if (virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) != 0) {
            lxcError(dom->conn, dom, VIR_ERR_INTERNAL_ERROR,
                     _("Unable to get cgroup for %s\n"), vm->def->name);
            goto cleanup;
        }

        if (virCgroupSetMemory(cgroup, newmem) < 0) {
            lxcError(dom->conn, dom, VIR_ERR_OPERATION_FAILED,
                     "%s", _("cannot set memory for domain"));
            goto cleanup;
        }
    } else {
        vm->def->memory = newmem;
    }
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (cgroup)
        virCgroupFree(&cgroup);
    return ret;
}

static char *lxcDomainDumpXML(virDomainPtr dom,
                              int flags)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *ret = NULL;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 "%s", _("no domain with matching uuid"));
        goto cleanup;
    }

    ret = virDomainDefFormat(dom->conn,
                             (flags & VIR_DOMAIN_XML_INACTIVE) &&
                             vm->newDef ? vm->newDef : vm->def,
                             flags);

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}


/**
 * lxcVmCleanup:
 * @vm: Ptr to VM to clean up
 *
 * waitpid() on the container process.  kill and wait the tty process
 * This is called by both lxcDomainDestroy and lxcSigHandler when a
 * container exits.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcVMCleanup(virConnectPtr conn,
                        lxc_driver_t *driver,
                        virDomainObjPtr  vm)
{
    int rc = -1;
    int waitRc;
    int childStatus = -1;
    virCgroupPtr cgroup;
    int i;

    while (((waitRc = waitpid(vm->pid, &childStatus, 0)) == -1) &&
           errno == EINTR)
        ; /* empty */

    if ((waitRc != vm->pid) && (errno != ECHILD)) {
        virReportSystemError(conn, errno,
                             _("waitpid failed to wait for container %d: %d"),
                             vm->pid, waitRc);
    }

    rc = 0;

    if (WIFEXITED(childStatus)) {
        rc = WEXITSTATUS(childStatus);
        DEBUG("container exited with rc: %d", rc);
    }

    virEventRemoveHandle(vm->monitorWatch);
    close(vm->monitor);

    virFileDeletePid(driver->stateDir, vm->def->name);
    virDomainDeleteConfig(conn, driver->stateDir, NULL, vm);

    vm->state = VIR_DOMAIN_SHUTOFF;
    vm->pid = -1;
    vm->def->id = -1;
    vm->monitor = -1;

    for (i = 0 ; i < vm->def->nnets ; i++) {
        vethInterfaceUpOrDown(vm->def->nets[i]->ifname, 0);
        vethDelete(vm->def->nets[i]->ifname);
    }

    if (driver->cgroup &&
        virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) == 0) {
        virCgroupRemove(cgroup);
        virCgroupFree(&cgroup);
    }

    if (vm->newDef) {
        virDomainDefFree(vm->def);
        vm->def = vm->newDef;
        vm->def->id = -1;
        vm->newDef = NULL;
    }

    return rc;
}

/**
 * lxcSetupInterfaces:
 * @def: pointer to virtual machine structure
 *
 * Sets up the container interfaces by creating the veth device pairs and
 * attaching the parent end to the appropriate bridge.  The container end
 * will moved into the container namespace later after clone has been called.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcSetupInterfaces(virConnectPtr conn,
                              virDomainDefPtr def,
                              unsigned int *nveths,
                              char ***veths)
{
    int rc = -1, i;
    char *bridge = NULL;
    char parentVeth[PATH_MAX] = "";
    char containerVeth[PATH_MAX] = "";
    brControl *brctl = NULL;

    if (brInit(&brctl) != 0)
        return -1;

    for (i = 0 ; i < def->nnets ; i++) {
        switch (def->nets[i]->type) {
        case VIR_DOMAIN_NET_TYPE_NETWORK:
        {
            virNetworkPtr network = virNetworkLookupByName(conn,
                                                           def->nets[i]->data.network.name);
            if (!network) {
                goto error_exit;
            }

            bridge = virNetworkGetBridgeName(network);

            virNetworkFree(network);
            break;
        }
        case VIR_DOMAIN_NET_TYPE_BRIDGE:
            bridge = def->nets[i]->data.bridge.brname;
            break;
        }

        DEBUG("bridge: %s", bridge);
        if (NULL == bridge) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     "%s", _("failed to get bridge for interface"));
            goto error_exit;
        }

        DEBUG0("calling vethCreate()");
        if (NULL != def->nets[i]->ifname) {
            strcpy(parentVeth, def->nets[i]->ifname);
        }
        DEBUG("parentVeth: %s, containerVeth: %s", parentVeth, containerVeth);
        if (0 != (rc = vethCreate(parentVeth, PATH_MAX, containerVeth, PATH_MAX))) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("failed to create veth device pair: %d"), rc);
            goto error_exit;
        }
        if (NULL == def->nets[i]->ifname) {
            def->nets[i]->ifname = strdup(parentVeth);
        }
        if (VIR_REALLOC_N(*veths, (*nveths)+1) < 0)
            goto error_exit;
        if (((*veths)[(*nveths)++] = strdup(containerVeth)) == NULL)
            goto error_exit;

        if (NULL == def->nets[i]->ifname) {
            lxcError(NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                     "%s", _("failed to allocate veth names"));
            goto error_exit;
        }

        if (def->nets[i]->mac) {
            char macaddr[VIR_MAC_STRING_BUFLEN];
            virFormatMacAddr(def->nets[i]->mac, macaddr);
            if (0 != (rc = setMacAddr(containerVeth, macaddr))) {
                virReportSystemError(conn, rc,
                                     _("failed to set %s to %s"),
                                     macaddr, containerVeth);
                goto error_exit;
            }
        }

        if (0 != (rc = brAddInterface(brctl, bridge, parentVeth))) {
            virReportSystemError(conn, rc,
                                 _("failed to add %s device to %s"),
                                 parentVeth, bridge);
            goto error_exit;
        }

        if (0 != (rc = vethInterfaceUpOrDown(parentVeth, 1))) {
            virReportSystemError(conn, rc, "%s",
                                 _("failed to enable parent ns veth device"));
            goto error_exit;
        }

    }

    rc = 0;

error_exit:
    brShutdown(brctl);
    return rc;
}


static int lxcMonitorClient(virConnectPtr conn,
                            lxc_driver_t * driver,
                            virDomainObjPtr vm)
{
    char *sockpath = NULL;
    int fd;
    struct sockaddr_un addr;

    if (virAsprintf(&sockpath, "%s/%s.sock",
                    driver->stateDir, vm->def->name) < 0) {
        virReportOOMError(conn);
        return -1;
    }

    if ((fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
        virReportSystemError(conn, errno, "%s",
                             _("failed to create client socket"));
        goto error;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (virStrcpyStatic(addr.sun_path, sockpath) == NULL) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("Socket path %s too big for destination"), sockpath);
        goto error;
    }

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        virReportSystemError(conn, errno, "%s",
                             _("failed to connect to client socket"));
        goto error;
    }

    VIR_FREE(sockpath);
    return fd;

error:
    VIR_FREE(sockpath);
    if (fd != -1)
        close(fd);
    return -1;
}


static int lxcVmTerminate(virConnectPtr conn,
                          lxc_driver_t *driver,
                          virDomainObjPtr vm,
                          int signum)
{
    if (signum == 0)
        signum = SIGINT;

    if (vm->pid <= 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("invalid PID %d for container"), vm->pid);
        return -1;
    }

    if (kill(vm->pid, signum) < 0) {
        if (errno != ESRCH) {
            virReportSystemError(conn, errno,
                                 _("failed to kill pid %d"),
                                 vm->pid);
            return -1;
        }
    }

    vm->state = VIR_DOMAIN_SHUTDOWN;

    return lxcVMCleanup(conn, driver, vm);
}

static void lxcMonitorEvent(int watch,
                            int fd,
                            int events ATTRIBUTE_UNUSED,
                            void *data)
{
    lxc_driver_t *driver = lxc_driver;
    virDomainObjPtr vm = data;
    virDomainEventPtr event = NULL;

    lxcDriverLock(driver);
    virDomainObjLock(vm);
    lxcDriverUnlock(driver);

    if (vm->monitor != fd || vm->monitorWatch != watch) {
        virEventRemoveHandle(watch);
        goto cleanup;
    }

    if (lxcVmTerminate(NULL, driver, vm, SIGINT) < 0) {
        virEventRemoveHandle(watch);
    } else {
        event = virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_STOPPED,
                                         VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN);
    }
    if (!vm->persistent) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
    }

cleanup:
    virDomainObjUnlock(vm);
    if (event) {
        lxcDriverLock(driver);
        lxcDomainEventQueue(driver, event);
        lxcDriverUnlock(driver);
    }
}


static int lxcControllerStart(virConnectPtr conn,
                              virDomainObjPtr vm,
                              int nveths,
                              char **veths,
                              int appPty,
                              int logfd)
{
    int i;
    int rc;
    int largc = 0, larga = 0;
    const char **largv = NULL;
    int lenvc = 0, lenva = 0;
    const char **lenv = NULL;
    char *filterstr;
    char *outputstr;
    char *tmp;
    int log_level;
    pid_t child;
    int status;
    fd_set keepfd;
    char appPtyStr[30];
    const char *emulator;
    lxc_driver_t *driver = conn->privateData;

    FD_ZERO(&keepfd);

#define ADD_ARG_SPACE                                                   \
    do { \
        if (largc == larga) {                                           \
            larga += 10;                                                \
            if (VIR_REALLOC_N(largv, larga) < 0)                        \
                goto no_memory;                                         \
        }                                                               \
    } while (0)

#define ADD_ARG(thisarg)                                                \
    do {                                                                \
        ADD_ARG_SPACE;                                                  \
        largv[largc++] = thisarg;                                       \
    } while (0)

#define ADD_ARG_LIT(thisarg)                                            \
    do {                                                                \
        ADD_ARG_SPACE;                                                  \
        if ((largv[largc++] = strdup(thisarg)) == NULL)                 \
            goto no_memory;                                             \
    } while (0)

#define ADD_ENV_SPACE                                                   \
    do {                                                                \
        if (lenvc == lenva) {                                           \
            lenva += 10;                                                \
            if (VIR_REALLOC_N(lenv, lenva) < 0)                         \
                goto no_memory;                                         \
        }                                                               \
    } while (0)

#define ADD_ENV(thisarg)                                                \
    do {                                                                \
        ADD_ENV_SPACE;                                                  \
        lenv[lenvc++] = thisarg;                                        \
    } while (0)

#define ADD_ENV_PAIR(envname, val)                                      \
    do {                                                                \
        char *envval;                                                   \
        ADD_ENV_SPACE;                                                  \
        if (virAsprintf(&envval, "%s=%s", envname, val) < 0)            \
            goto no_memory;                                             \
        lenv[lenvc++] = envval;                                         \
    } while (0)

#define ADD_ENV_COPY(envname)                                           \
    do {                                                                \
        char *val = getenv(envname);                                    \
        if (val != NULL) {                                              \
            ADD_ENV_PAIR(envname, val);                                 \
        }                                                               \
    } while (0)

    /*
     * The controller may call ip command, so we have to remain PATH.
     */
    ADD_ENV_COPY("PATH");

    log_level = virLogGetDefaultPriority();
    if (virAsprintf(&tmp, "LIBVIRT_DEBUG=%d", log_level) < 0)
        goto no_memory;
    ADD_ENV(tmp);

    if (virLogGetNbFilters() > 0) {
        filterstr = virLogGetFilters();
        if (!filterstr)
            goto no_memory;
        ADD_ENV_PAIR("LIBVIRT_LOG_FILTERS", filterstr);
        VIR_FREE(filterstr);
    }

    if (driver->log_libvirtd) {
        if (virLogGetNbOutputs() > 0) {
            outputstr = virLogGetOutputs();
            if (!outputstr)
                goto no_memory;
            ADD_ENV_PAIR("LIBVIRT_LOG_OUTPUTS", outputstr);
            VIR_FREE(outputstr);
        }
    } else {
        if (virAsprintf(&tmp, "LIBVIRT_LOG_OUTPUTS=%d:stderr", log_level) < 0)
            goto no_memory;
        ADD_ENV(tmp);
    }

    ADD_ENV(NULL);

    snprintf(appPtyStr, sizeof(appPtyStr), "%d", appPty);

    emulator = vm->def->emulator;

    ADD_ARG_LIT(emulator);
    ADD_ARG_LIT("--name");
    ADD_ARG_LIT(vm->def->name);
    ADD_ARG_LIT("--console");
    ADD_ARG_LIT(appPtyStr);
    ADD_ARG_LIT("--background");

    for (i = 0 ; i < nveths ; i++) {
        ADD_ARG_LIT("--veth");
        ADD_ARG_LIT(veths[i]);
    }

    ADD_ARG(NULL);

    FD_SET(appPty, &keepfd);

    if (virExec(conn, largv, lenv, &keepfd, &child,
                -1, &logfd, &logfd,
                VIR_EXEC_NONE) < 0)
        goto cleanup;

    /* We now wait for the process to exit - the controller
     * will fork() itself into the background - waiting for
     * it to exit thus guarentees it has written its pidfile
     */
    while ((rc = waitpid(child, &status, 0) == -1) && errno == EINTR);
    if (rc == -1) {
        virReportSystemError(conn, errno,
                             _("cannot wait for '%s'"),
                             largv[0]);
        goto cleanup;
    }

    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("container '%s' unexpectedly shutdown during startup"),
                 largv[0]);
        goto cleanup;
    }

#undef ADD_ARG
#undef ADD_ARG_LIT
#undef ADD_ARG_SPACE
#undef ADD_ENV_SPACE
#undef ADD_ENV_PAIR

    return 0;

no_memory:
    virReportOOMError(conn);
cleanup:
    if (largv) {
        for (i = 0 ; i < largc ; i++)
            VIR_FREE(largv[i]);
        VIR_FREE(largv);
    }
    if (lenv) {
        for (i=0 ; i < lenvc ; i++)
            VIR_FREE(lenv[i]);
        VIR_FREE(lenv);
    }
    return -1;
}


/**
 * lxcVmStart:
 * @conn: pointer to connection
 * @driver: pointer to driver structure
 * @vm: pointer to virtual machine structure
 *
 * Starts a vm
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcVmStart(virConnectPtr conn,
                      lxc_driver_t * driver,
                      virDomainObjPtr  vm)
{
    int rc = -1, r;
    unsigned int i;
    int parentTty;
    char *parentTtyPath = NULL;
    char *logfile = NULL;
    int logfd = -1;
    unsigned int nveths = 0;
    char **veths = NULL;

    if ((r = virFileMakePath(driver->logDir)) < 0) {
        virReportSystemError(conn, r,
                             _("cannot create log directory '%s'"),
                             driver->logDir);
        return -1;
    }

    if (virAsprintf(&logfile, "%s/%s.log",
                    driver->logDir, vm->def->name) < 0) {
        virReportOOMError(conn);
        return -1;
    }

    /* open parent tty */
    if (virFileOpenTty(&parentTty, &parentTtyPath, 1) < 0) {
        virReportSystemError(conn, errno, "%s",
                             _("failed to allocate tty"));
        goto cleanup;
    }
    if (vm->def->console &&
        vm->def->console->type == VIR_DOMAIN_CHR_TYPE_PTY) {
        VIR_FREE(vm->def->console->data.file.path);
        vm->def->console->data.file.path = parentTtyPath;
    } else {
        VIR_FREE(parentTtyPath);
    }

    if (lxcSetupInterfaces(conn, vm->def, &nveths, &veths) != 0)
        goto cleanup;

    /* Persist the live configuration now we have veth & tty info */
    if (virDomainSaveConfig(conn, driver->stateDir, vm->def) < 0)
        goto cleanup;

    if ((logfd = open(logfile, O_WRONLY | O_APPEND | O_CREAT,
             S_IRUSR|S_IWUSR)) < 0) {
        virReportSystemError(conn, errno,
                             _("failed to open '%s'"),
                             logfile);
        goto cleanup;
    }

    if (lxcControllerStart(conn,
                           vm,
                           nveths, veths,
                           parentTty, logfd) < 0)
        goto cleanup;

    /* Connect to the controller as a client *first* because
     * this will block until the child has written their
     * pid file out to disk */
    if ((vm->monitor = lxcMonitorClient(conn, driver, vm)) < 0)
        goto cleanup;

    /* And get its pid */
    if ((r = virFileReadPid(driver->stateDir, vm->def->name, &vm->pid)) != 0) {
        virReportSystemError(conn, r,
                             _("Failed to read pid file %s/%s.pid"),
                             driver->stateDir, vm->def->name);
        goto cleanup;
    }

    vm->def->id = vm->pid;
    vm->state = VIR_DOMAIN_RUNNING;

    if ((vm->monitorWatch = virEventAddHandle(
             vm->monitor,
             VIR_EVENT_HANDLE_ERROR | VIR_EVENT_HANDLE_HANGUP,
             lxcMonitorEvent,
             driver, NULL)) < 0) {
        lxcVmTerminate(conn, driver, vm, 0);
        goto cleanup;
    }

    rc = 0;

cleanup:
    for (i = 0 ; i < nveths ; i++) {
        if (rc != 0)
            vethDelete(veths[i]);
        VIR_FREE(veths[i]);
    }
    if (rc != 0 && vm->monitor != -1) {
        close(vm->monitor);
        vm->monitor = -1;
    }
    if (parentTty != -1)
        close(parentTty);
    if (logfd != -1)
        close(logfd);
    VIR_FREE(logfile);
    return rc;
}

/**
 * lxcDomainStart:
 * @dom: domain to start
 *
 * Looks up domain and starts it.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcDomainStart(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByName(&driver->domains, dom->name);
    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 _("no domain named %s"), dom->name);
        goto cleanup;
    }

    if ((vm->def->nets != NULL) && !(driver->have_netns)) {
        lxcError(dom->conn, NULL, VIR_ERR_NO_SUPPORT,
                 "%s", _("System lacks NETNS support"));
        goto cleanup;
    }

    ret = lxcVmStart(dom->conn, driver, vm);

    if (ret == 0)
        event = virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_STARTED,
                                         VIR_DOMAIN_EVENT_STARTED_BOOTED);

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        lxcDomainEventQueue(driver, event);
    lxcDriverUnlock(driver);
    return ret;
}

/**
 * lxcDomainCreateAndStart:
 * @conn: pointer to connection
 * @xml: XML definition of domain
 * @flags: Unused
 *
 * Creates a domain based on xml and starts it
 *
 * Returns 0 on success or -1 in case of error
 */
static virDomainPtr
lxcDomainCreateAndStart(virConnectPtr conn,
                        const char *xml,
                        unsigned int flags ATTRIBUTE_UNUSED) {
    lxc_driver_t *driver = conn->privateData;
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def;
    virDomainPtr dom = NULL;
    virDomainEventPtr event = NULL;

    lxcDriverLock(driver);
    if (!(def = virDomainDefParseString(conn, driver->caps, xml,
                                        VIR_DOMAIN_XML_INACTIVE)))
        goto cleanup;

    /* See if a VM with matching UUID already exists */
    vm = virDomainFindByUUID(&driver->domains, def->uuid);
    if (vm) {
        /* UUID matches, but if names don't match, refuse it */
        if (STRNEQ(vm->def->name, def->name)) {
            char uuidstr[VIR_UUID_STRING_BUFLEN];
            virUUIDFormat(vm->def->uuid, uuidstr);
            lxcError(conn, NULL, VIR_ERR_OPERATION_FAILED,
                     _("domain '%s' is already defined with uuid %s"),
                     vm->def->name, uuidstr);
            goto cleanup;
        }

        /* UUID & name match, but if VM is already active, refuse it */
        if (virDomainObjIsActive(vm)) {
            lxcError(conn, NULL, VIR_ERR_OPERATION_FAILED,
                     _("domain is already active as '%s'"), vm->def->name);
            goto cleanup;
        }
        virDomainObjUnlock(vm);
    } else {
        /* UUID does not match, but if a name matches, refuse it */
        vm = virDomainFindByName(&driver->domains, def->name);
        if (vm) {
            char uuidstr[VIR_UUID_STRING_BUFLEN];
            virUUIDFormat(vm->def->uuid, uuidstr);
            lxcError(conn, NULL, VIR_ERR_OPERATION_FAILED,
                     _("domain '%s' is already defined with uuid %s"),
                     def->name, uuidstr);
            goto cleanup;
        }
    }

    if ((def->nets != NULL) && !(driver->have_netns)) {
        lxcError(conn, NULL, VIR_ERR_NO_SUPPORT,
                 "%s", _("System lacks NETNS support"));
        goto cleanup;
    }


    if (!(vm = virDomainAssignDef(conn, driver->caps,
                                  &driver->domains, def)))
        goto cleanup;
    def = NULL;

    if (lxcVmStart(conn, driver, vm) < 0) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
        goto cleanup;
    }

    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STARTED,
                                     VIR_DOMAIN_EVENT_STARTED_BOOTED);

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    virDomainDefFree(def);
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        lxcDomainEventQueue(driver, event);
    lxcDriverUnlock(driver);
    return dom;
}

/**
 * lxcDomainShutdown:
 * @dom: Ptr to domain to shutdown
 *
 * Sends SIGINT to container root process to request it to shutdown
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcDomainShutdown(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByID(&driver->domains, dom->id);
    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 _("no domain with id %d"), dom->id);
        goto cleanup;
    }

    ret = lxcVmTerminate(dom->conn, driver, vm, 0);
    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STOPPED,
                                     VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN);
    if (!vm->persistent) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
    }

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        lxcDomainEventQueue(driver, event);
    lxcDriverUnlock(driver);
    return ret;
}


static int
lxcDomainEventRegister (virConnectPtr conn,
                        virConnectDomainEventCallback callback,
                        void *opaque,
                        virFreeCallback freecb)
{
    lxc_driver_t *driver = conn->privateData;
    int ret;

    lxcDriverLock(driver);
    ret = virDomainEventCallbackListAdd(conn, driver->domainEventCallbacks,
                                        callback, opaque, freecb);
    lxcDriverUnlock(driver);

    return ret;
}

static int
lxcDomainEventDeregister (virConnectPtr conn,
                          virConnectDomainEventCallback callback)
{
    lxc_driver_t *driver = conn->privateData;
    int ret;

    lxcDriverLock(driver);
    if (driver->domainEventDispatching)
        ret = virDomainEventCallbackListMarkDelete(conn, driver->domainEventCallbacks,
                                                   callback);
    else
        ret = virDomainEventCallbackListRemove(conn, driver->domainEventCallbacks,
                                               callback);
    lxcDriverUnlock(driver);

    return ret;
}

static void lxcDomainEventDispatchFunc(virConnectPtr conn,
                                       virDomainEventPtr event,
                                       virConnectDomainEventCallback cb,
                                       void *cbopaque,
                                       void *opaque)
{
    lxc_driver_t *driver = opaque;

    /* Drop the lock whle dispatching, for sake of re-entrancy */
    lxcDriverUnlock(driver);
    virDomainEventDispatchDefaultFunc(conn, event, cb, cbopaque, NULL);
    lxcDriverLock(driver);
}


static void lxcDomainEventFlush(int timer ATTRIBUTE_UNUSED, void *opaque)
{
    lxc_driver_t *driver = opaque;
    virDomainEventQueue tempQueue;

    lxcDriverLock(driver);

    driver->domainEventDispatching = 1;

    /* Copy the queue, so we're reentrant safe */
    tempQueue.count = driver->domainEventQueue->count;
    tempQueue.events = driver->domainEventQueue->events;
    driver->domainEventQueue->count = 0;
    driver->domainEventQueue->events = NULL;

    virEventUpdateTimeout(driver->domainEventTimer, -1);
    virDomainEventQueueDispatch(&tempQueue,
                                driver->domainEventCallbacks,
                                lxcDomainEventDispatchFunc,
                                driver);

    /* Purge any deleted callbacks */
    virDomainEventCallbackListPurgeMarked(driver->domainEventCallbacks);

    driver->domainEventDispatching = 0;
    lxcDriverUnlock(driver);
}


/* driver must be locked before calling */
static void lxcDomainEventQueue(lxc_driver_t *driver,
                                 virDomainEventPtr event)
{
    if (virDomainEventQueuePush(driver->domainEventQueue,
                                event) < 0)
        virDomainEventFree(event);
    if (lxc_driver->domainEventQueue->count == 1)
        virEventUpdateTimeout(driver->domainEventTimer, 0);
}

/**
 * lxcDomainDestroy:
 * @dom: Ptr to domain to destroy
 *
 * Sends SIGKILL to container root process to terminate the container
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcDomainDestroy(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByID(&driver->domains, dom->id);
    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 _("no domain with id %d"), dom->id);
        goto cleanup;
    }

    ret = lxcVmTerminate(dom->conn, driver, vm, SIGKILL);
    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STOPPED,
                                     VIR_DOMAIN_EVENT_STOPPED_DESTROYED);
    if (!vm->persistent) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
    }

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        lxcDomainEventQueue(driver, event);
    lxcDriverUnlock(driver);
    return ret;
}

static int lxcCheckNetNsSupport(void)
{
    const char *argv[] = {"ip", "link", "set", "lo", "netns", "-1", NULL};
    int ip_rc;

    if (virRun(NULL, argv, &ip_rc) < 0 ||
        !(WIFEXITED(ip_rc) && (WEXITSTATUS(ip_rc) != 255)))
        return 0;

    if (lxcContainerAvailable(LXC_CONTAINER_FEATURE_NET) < 0)
        return 0;

    return 1;
}


struct lxcAutostartData {
    lxc_driver_t *driver;
    virConnectPtr conn;
};

static void
lxcAutostartDomain(void *payload, const char *name ATTRIBUTE_UNUSED, void *opaque)
{
    virDomainObjPtr vm = payload;
    const struct lxcAutostartData *data = opaque;

    virDomainObjLock(vm);
    if (vm->autostart &&
        !virDomainObjIsActive(vm)) {
        int ret = lxcVmStart(data->conn, data->driver, vm);
        if (ret < 0) {
            virErrorPtr err = virGetLastError();
            VIR_ERROR(_("Failed to autostart VM '%s': %s\n"),
                      vm->def->name,
                      err ? err->message : "");
        } else {
            virDomainEventPtr event =
                virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_STARTED,
                                         VIR_DOMAIN_EVENT_STARTED_BOOTED);
            if (event)
                lxcDomainEventQueue(data->driver, event);
        }
    }
    virDomainObjUnlock(vm);
}

static void
lxcAutostartConfigs(lxc_driver_t *driver) {
    /* XXX: Figure out a better way todo this. The domain
     * startup code needs a connection handle in order
     * to lookup the bridge associated with a virtual
     * network
     */
    virConnectPtr conn = virConnectOpen("lxc:///");
    /* Ignoring NULL conn which is mostly harmless here */

    struct lxcAutostartData data = { driver, conn };

    lxcDriverLock(driver);
    virHashForEach(driver->domains.objs, lxcAutostartDomain, &data);
    lxcDriverUnlock(driver);

    if (conn)
        virConnectClose(conn);
}

static void
lxcReconnectVM(void *payload, const char *name ATTRIBUTE_UNUSED, void *opaque)
{
    virDomainObjPtr vm = payload;
    lxc_driver_t *driver = opaque;
    char *config = NULL;
    virDomainDefPtr tmp;

    virDomainObjLock(vm);
    if ((vm->monitor = lxcMonitorClient(NULL, driver, vm)) < 0) {
        goto cleanup;
    }

    /* Read pid from controller */
    if ((virFileReadPid(lxc_driver->stateDir, vm->def->name, &vm->pid)) != 0) {
        close(vm->monitor);
        vm->monitor = -1;
        goto cleanup;
    }

    if ((config = virDomainConfigFile(NULL,
                                      driver->stateDir,
                                      vm->def->name)) == NULL)
        goto cleanup;

    /* Try and load the live config */
    tmp = virDomainDefParseFile(NULL, driver->caps, config, 0);
    VIR_FREE(config);
    if (tmp) {
        vm->newDef = vm->def;
        vm->def = tmp;
    }

    if (vm->pid != 0) {
        vm->def->id = vm->pid;
        vm->state = VIR_DOMAIN_RUNNING;
    } else {
        vm->def->id = -1;
        close(vm->monitor);
        vm->monitor = -1;
    }

cleanup:
    virDomainObjUnlock(vm);
}


static int lxcStartup(int privileged)
{
    char *ld;
    int rc;

    /* Valgrind gets very annoyed when we clone containers, so
     * disable LXC when under valgrind
     * XXX remove this when valgrind is fixed
     */
    ld = getenv("LD_PRELOAD");
    if (ld && strstr(ld, "vgpreload"))
        return -1;

    /* Check that the user is root */
    if (!privileged) {
        return -1;
    }

    if (VIR_ALLOC(lxc_driver) < 0) {
        return -1;
    }
    if (virMutexInit(&lxc_driver->lock) < 0) {
        VIR_FREE(lxc_driver);
        return -1;
    }
    lxcDriverLock(lxc_driver);

    /* Check that this is a container enabled kernel */
    if (lxcContainerAvailable(0) < 0) {
        VIR_INFO0("LXC support not available in this kernel, disabling driver");
        goto cleanup;
    }

    if (virDomainObjListInit(&lxc_driver->domains) < 0)
        goto cleanup;

    if (VIR_ALLOC(lxc_driver->domainEventCallbacks) < 0)
        goto cleanup;
    if (!(lxc_driver->domainEventQueue = virDomainEventQueueNew()))
        goto cleanup;

    if ((lxc_driver->domainEventTimer =
         virEventAddTimeout(-1, lxcDomainEventFlush, lxc_driver, NULL)) < 0)
        goto cleanup;

    lxc_driver->log_libvirtd = 0; /* by default log to container logfile */
    lxc_driver->have_netns = lxcCheckNetNsSupport();

    rc = virCgroupForDriver("lxc", &lxc_driver->cgroup, privileged, 1);
    if (rc < 0) {
        char buf[1024];
        VIR_WARN("Unable to create cgroup for driver: %s",
                 virStrerror(-rc, buf, sizeof(buf)));
    }

    /* Call function to load lxc driver configuration information */
    if (lxcLoadDriverConfig(lxc_driver) < 0)
        goto cleanup;

    if ((lxc_driver->caps = lxcCapsInit()) == NULL)
        goto cleanup;

    if (virDomainLoadAllConfigs(NULL,
                                lxc_driver->caps,
                                &lxc_driver->domains,
                                lxc_driver->configDir,
                                lxc_driver->autostartDir,
                                0, NULL, NULL) < 0)
        goto cleanup;

    virHashForEach(lxc_driver->domains.objs, lxcReconnectVM, lxc_driver);

    lxcDriverUnlock(lxc_driver);
    return 0;

cleanup:
    lxcDriverUnlock(lxc_driver);
    lxcShutdown();
    return -1;
}

static void lxcNotifyLoadDomain(virDomainObjPtr vm, int newVM, void *opaque)
{
    lxc_driver_t *driver = opaque;

    if (newVM) {
        virDomainEventPtr event =
            virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_DEFINED,
                                     VIR_DOMAIN_EVENT_DEFINED_ADDED);
        if (event)
            lxcDomainEventQueue(driver, event);
    }
}

/**
 * lxcReload:
 *
 * Function to restart the LXC driver, it will recheck the configuration
 * files and perform autostart
 */
static int
lxcReload(void) {
    if (!lxc_driver)
        return 0;

    lxcDriverLock(lxc_driver);
    virDomainLoadAllConfigs(NULL,
                            lxc_driver->caps,
                            &lxc_driver->domains,
                            lxc_driver->configDir,
                            lxc_driver->autostartDir,
                            0, lxcNotifyLoadDomain, lxc_driver);
    lxcDriverUnlock(lxc_driver);

    lxcAutostartConfigs(lxc_driver);

    return 0;
}

static int lxcShutdown(void)
{
    if (lxc_driver == NULL)
        return(-1);

    lxcDriverLock(lxc_driver);
    virDomainObjListDeinit(&lxc_driver->domains);

    virDomainEventCallbackListFree(lxc_driver->domainEventCallbacks);
    virDomainEventQueueFree(lxc_driver->domainEventQueue);

    if (lxc_driver->domainEventTimer != -1)
        virEventRemoveTimeout(lxc_driver->domainEventTimer);

    virCapabilitiesFree(lxc_driver->caps);
    VIR_FREE(lxc_driver->configDir);
    VIR_FREE(lxc_driver->autostartDir);
    VIR_FREE(lxc_driver->stateDir);
    VIR_FREE(lxc_driver->logDir);
    lxcDriverUnlock(lxc_driver);
    virMutexDestroy(&lxc_driver->lock);
    VIR_FREE(lxc_driver);

    return 0;
}

/**
 * lxcActive:
 *
 * Checks if the LXC daemon is active, i.e. has an active domain
 *
 * Returns 1 if active, 0 otherwise
 */
static int
lxcActive(void) {
    int active;

    if (lxc_driver == NULL)
        return(0);

    lxcDriverLock(lxc_driver);
    active = virDomainObjListNumOfDomains(&lxc_driver->domains, 1);
    lxcDriverUnlock(lxc_driver);

    return active;
}

static int lxcVersion(virConnectPtr conn, unsigned long *version)
{
    struct utsname ver;
    int maj;
    int min;
    int rev;

    uname(&ver);

    if (sscanf(ver.release, "%i.%i.%i", &maj, &min, &rev) != 3) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("Unknown release: %s"), ver.release);
        return -1;
    }

    *version = (maj * 1000 * 1000) + (min * 1000) + rev;

    return 0;
}

static char *lxcGetSchedulerType(virDomainPtr domain ATTRIBUTE_UNUSED,
                                 int *nparams)
{
    if (nparams)
        *nparams = 1;

    return strdup("posix");
}

static int lxcSetSchedulerParameters(virDomainPtr domain,
                                     virSchedParameterPtr params,
                                     int nparams)
{
    lxc_driver_t *driver = domain->conn->privateData;
    int i;
    virCgroupPtr group = NULL;
    virDomainObjPtr vm = NULL;
    int ret = -1;

    if (driver->cgroup == NULL)
        return -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, domain->uuid);

    if (vm == NULL) {
        lxcError(NULL, domain, VIR_ERR_INTERNAL_ERROR,
                 _("No such domain %s"), domain->uuid);
        goto cleanup;
    }

    if (virCgroupForDomain(driver->cgroup, vm->def->name, &group, 0) != 0)
        goto cleanup;

    for (i = 0; i < nparams; i++) {
        virSchedParameterPtr param = &params[i];
        if (param->type != VIR_DOMAIN_SCHED_FIELD_ULLONG) {
            lxcError(NULL, domain, VIR_ERR_INVALID_ARG,
                     _("invalid type for cpu_shares tunable, expected a 'ullong'"));
            goto cleanup;
        }

        if (STREQ(param->field, "cpu_shares")) {
            if (virCgroupSetCpuShares(group, params[i].value.ul) != 0)
                goto cleanup;
        } else {
            lxcError(NULL, domain, VIR_ERR_INVALID_ARG,
                     _("Invalid parameter `%s'"), param->field);
            goto cleanup;
        }
    }
    ret = 0;

cleanup:
    lxcDriverUnlock(driver);
    virCgroupFree(&group);
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int lxcGetSchedulerParameters(virDomainPtr domain,
                                     virSchedParameterPtr params,
                                     int *nparams)
{
    lxc_driver_t *driver = domain->conn->privateData;
    virCgroupPtr group = NULL;
    virDomainObjPtr vm = NULL;
    unsigned long long val;
    int ret = -1;

    if (driver->cgroup == NULL)
        return -1;

    if ((*nparams) != 1) {
        lxcError(NULL, domain, VIR_ERR_INVALID_ARG,
                 "%s", _("Invalid parameter count"));
        return -1;
    }

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, domain->uuid);

    if (vm == NULL) {
        lxcError(NULL, domain, VIR_ERR_INTERNAL_ERROR,
                 _("No such domain %s"), domain->uuid);
        goto cleanup;
    }

    if (virCgroupForDomain(driver->cgroup, vm->def->name, &group, 0) != 0)
        goto cleanup;

    if (virCgroupGetCpuShares(group, &val) != 0)
        goto cleanup;
    params[0].value.ul = val;
    if (virStrcpyStatic(params[0].field, "cpu_shares") == NULL) {
        lxcError(NULL, domain, VIR_ERR_INTERNAL_ERROR,
                 "%s", _("Field cpu_shares too big for destination"));
        goto cleanup;
    }
    params[0].type = VIR_DOMAIN_SCHED_FIELD_ULLONG;

    ret = 0;

cleanup:
    lxcDriverUnlock(driver);
    virCgroupFree(&group);
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int lxcDomainGetAutostart(virDomainPtr dom,
                                   int *autostart) {
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(dom->conn, dom, VIR_ERR_NO_DOMAIN,
                 _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    *autostart = vm->autostart;
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int lxcDomainSetAutostart(virDomainPtr dom,
                                   int autostart) {
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *configFile = NULL, *autostartLink = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(dom->conn, dom, VIR_ERR_NO_DOMAIN,
                 _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!vm->persistent) {
        lxcError(dom->conn, dom, VIR_ERR_INTERNAL_ERROR,
                 "%s", _("cannot set autostart for transient domain"));
        goto cleanup;
    }

    autostart = (autostart != 0);

    if (vm->autostart != autostart) {
        if ((configFile = virDomainConfigFile(dom->conn, driver->configDir, vm->def->name)) == NULL)
            goto cleanup;
        if ((autostartLink = virDomainConfigFile(dom->conn, driver->autostartDir, vm->def->name)) == NULL)
            goto cleanup;

        if (autostart) {
            int err;

            if ((err = virFileMakePath(driver->autostartDir))) {
                virReportSystemError(dom->conn, err,
                                     _("cannot create autostart directory %s"),
                                     driver->autostartDir);
                goto cleanup;
            }

            if (symlink(configFile, autostartLink) < 0) {
                virReportSystemError(dom->conn, errno,
                                     _("Failed to create symlink '%s to '%s'"),
                                     autostartLink, configFile);
                goto cleanup;
            }
        } else {
            if (unlink(autostartLink) < 0 && errno != ENOENT && errno != ENOTDIR) {
                virReportSystemError(dom->conn, errno,
                                     _("Failed to delete symlink '%s'"),
                                     autostartLink);
                goto cleanup;
            }
        }

        vm->autostart = autostart;
    }
    ret = 0;

cleanup:
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    if (vm)
        virDomainObjUnlock(vm);
    lxcDriverUnlock(driver);
    return ret;
}

static char *lxcGetHostname (virConnectPtr conn)
{
    char *result;

    result = virGetHostname();
    if (result == NULL) {
        virReportSystemError (conn, errno,
                              "%s", _("failed to determine host name"));
        return NULL;
    }
    /* Caller frees this string. */
    return result;
}

static int lxcFreezeContainer(lxc_driver_t *driver, virDomainObjPtr vm)
{
    int timeout = 1000; /* In milliseconds */
    int check_interval = 1; /* In milliseconds */
    int exp = 10;
    int waited_time = 0;
    int ret = -1;
    char *state = NULL;
    virCgroupPtr cgroup = NULL;

    if (!(driver->cgroup &&
        virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) == 0))
        return -1;

    while (waited_time < timeout) {
        int r;
        /*
         * Writing "FROZEN" to the "freezer.state" freezes the group,
         * i.e., the container, temporarily transiting "FREEZING" state.
         * Once the freezing is completed, the state of the group transits
         * to "FROZEN".
         * (see linux-2.6/Documentation/cgroups/freezer-subsystem.txt)
         */
        r = virCgroupSetFreezerState(cgroup, "FROZEN");

        /*
         * Returning EBUSY explicitly indicates that the group is
         * being freezed but incomplete and other errors are true
         * errors.
         */
        if (r < 0 && r != -EBUSY) {
            VIR_DEBUG("Writing freezer.state failed with errno: %d", r);
            goto error;
        }
        if (r == -EBUSY)
            VIR_DEBUG0("Writing freezer.state gets EBUSY");

        /*
         * Unfortunately, returning 0 (success) is likely to happen
         * even when the freezing has not been completed. Sometimes
         * the state of the group remains "FREEZING" like when
         * returning -EBUSY and even worse may never transit to
         * "FROZEN" even if writing "FROZEN" again.
         *
         * So we don't trust the return value anyway and always
         * decide that the freezing has been complete only with
         * the state actually transit to "FROZEN".
         */
        usleep(check_interval * 1000);

        r = virCgroupGetFreezerState(cgroup, &state);

        if (r < 0) {
            VIR_DEBUG("Reading freezer.state failed with errno: %d", r);
            goto error;
        }
        VIR_DEBUG("Read freezer.state: %s", state);

        if (STREQ(state, "FROZEN")) {
            ret = 0;
            goto cleanup;
        }

        waited_time += check_interval;
        /*
         * Increasing check_interval exponentially starting with
         * small initial value treats nicely two cases; One is
         * a container is under no load and waiting for long period
         * makes no sense. The other is under heavy load. The container
         * may stay longer time in FREEZING or never transit to FROZEN.
         * In that case, eager polling will just waste CPU time.
         */
        check_interval *= exp;
        VIR_FREE(state);
    }
    VIR_DEBUG0("lxcFreezeContainer timeout");
error:
    /*
     * If timeout or an error on reading the state occurs,
     * activate the group again and return an error.
     * This is likely to fall the group back again gracefully.
     */
    virCgroupSetFreezerState(cgroup, "THAWED");
    ret = -1;

cleanup:
    if (cgroup)
        virCgroupFree(&cgroup);
    VIR_FREE(state);
    return ret;
}

static int lxcDomainSuspend(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(dom->conn, dom, VIR_ERR_NO_DOMAIN,
                 _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        lxcError(dom->conn, dom, VIR_ERR_OPERATION_INVALID,
                         "%s", _("domain is not running"));
        goto cleanup;
    }

    if (vm->state != VIR_DOMAIN_PAUSED) {
        if (lxcFreezeContainer(driver, vm) < 0) {
            lxcError(dom->conn, dom, VIR_ERR_OPERATION_FAILED,
                             "%s", _("suspend operation failed"));
            goto cleanup;
        }
        vm->state = VIR_DOMAIN_PAUSED;

        event = virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_SUSPENDED,
                                         VIR_DOMAIN_EVENT_SUSPENDED_PAUSED);
    }

    if (virDomainSaveStatus(dom->conn, driver->stateDir, vm) < 0)
        goto cleanup;
    ret = 0;

cleanup:
    if (event)
        lxcDomainEventQueue(driver, event);
    if (vm)
        virDomainObjUnlock(vm);
    lxcDriverUnlock(driver);
    return ret;
}

static int lxcUnfreezeContainer(lxc_driver_t *driver, virDomainObjPtr vm)
{
    int ret;
    virCgroupPtr cgroup = NULL;

    if (!(driver->cgroup &&
        virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) == 0))
        return -1;

    ret = virCgroupSetFreezerState(cgroup, "THAWED");

    virCgroupFree(&cgroup);
    return ret;
}

static int lxcDomainResume(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(dom->conn, dom, VIR_ERR_NO_DOMAIN,
                 _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        lxcError(dom->conn, dom, VIR_ERR_OPERATION_INVALID,
                         "%s", _("domain is not running"));
        goto cleanup;
    }

    if (vm->state == VIR_DOMAIN_PAUSED) {
        if (lxcUnfreezeContainer(driver, vm) < 0) {
            lxcError(dom->conn, dom, VIR_ERR_OPERATION_FAILED,
                             "%s", _("resume operation failed"));
            goto cleanup;
        }
        vm->state = VIR_DOMAIN_RUNNING;

        event = virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_RESUMED,
                                         VIR_DOMAIN_EVENT_RESUMED_UNPAUSED);
    }

    if (virDomainSaveStatus(dom->conn, driver->stateDir, vm) < 0)
        goto cleanup;
    ret = 0;

cleanup:
    if (event)
        lxcDomainEventQueue(driver, event);
    if (vm)
        virDomainObjUnlock(vm);
    lxcDriverUnlock(driver);
    return ret;
}


/* Function Tables */
static virDriver lxcDriver = {
    VIR_DRV_LXC, /* the number virDrvNo */
    "LXC", /* the name of the driver */
    lxcOpen, /* open */
    lxcClose, /* close */
    NULL, /* supports_feature */
    NULL, /* type */
    lxcVersion, /* version */
    lxcGetHostname, /* getHostname */
    NULL, /* getMaxVcpus */
    nodeGetInfo, /* nodeGetInfo */
    lxcGetCapabilities, /* getCapabilities */
    lxcListDomains, /* listDomains */
    lxcNumDomains, /* numOfDomains */
    lxcDomainCreateAndStart, /* domainCreateXML */
    lxcDomainLookupByID, /* domainLookupByID */
    lxcDomainLookupByUUID, /* domainLookupByUUID */
    lxcDomainLookupByName, /* domainLookupByName */
    lxcDomainSuspend, /* domainSuspend */
    lxcDomainResume, /* domainResume */
    lxcDomainShutdown, /* domainShutdown */
    NULL, /* domainReboot */
    lxcDomainDestroy, /* domainDestroy */
    lxcGetOSType, /* domainGetOSType */
    lxcDomainGetMaxMemory, /* domainGetMaxMemory */
    lxcDomainSetMaxMemory, /* domainSetMaxMemory */
    lxcDomainSetMemory, /* domainSetMemory */
    lxcDomainGetInfo, /* domainGetInfo */
    NULL, /* domainSave */
    NULL, /* domainRestore */
    NULL, /* domainCoreDump */
    NULL, /* domainSetVcpus */
    NULL, /* domainPinVcpu */
    NULL, /* domainGetVcpus */
    NULL, /* domainGetMaxVcpus */
    NULL, /* domainGetSecurityLabel */
    NULL, /* nodeGetSecurityModel */
    lxcDomainDumpXML, /* domainDumpXML */
    NULL, /* domainXMLFromNative */
    NULL, /* domainXMLToNative */
    lxcListDefinedDomains, /* listDefinedDomains */
    lxcNumDefinedDomains, /* numOfDefinedDomains */
    lxcDomainStart, /* domainCreate */
    lxcDomainDefine, /* domainDefineXML */
    lxcDomainUndefine, /* domainUndefine */
    NULL, /* domainAttachDevice */
    NULL, /* domainDetachDevice */
    lxcDomainGetAutostart, /* domainGetAutostart */
    lxcDomainSetAutostart, /* domainSetAutostart */
    lxcGetSchedulerType, /* domainGetSchedulerType */
    lxcGetSchedulerParameters, /* domainGetSchedulerParameters */
    lxcSetSchedulerParameters, /* domainSetSchedulerParameters */
    NULL, /* domainMigratePrepare */
    NULL, /* domainMigratePerform */
    NULL, /* domainMigrateFinish */
    NULL, /* domainBlockStats */
    NULL, /* domainInterfaceStats */
    NULL, /* domainBlockPeek */
    NULL, /* domainMemoryPeek */
    nodeGetCellsFreeMemory, /* nodeGetCellsFreeMemory */
    nodeGetFreeMemory,  /* getFreeMemory */
    lxcDomainEventRegister, /* domainEventRegister */
    lxcDomainEventDeregister, /* domainEventDeregister */
    NULL, /* domainMigratePrepare2 */
    NULL, /* domainMigrateFinish2 */
    NULL, /* nodeDeviceDettach */
    NULL, /* nodeDeviceReAttach */
    NULL, /* nodeDeviceReset */
    NULL, /* domainMigratePrepareTunnel */
};

static virStateDriver lxcStateDriver = {
    .initialize = lxcStartup,
    .cleanup = lxcShutdown,
    .active = lxcActive,
    .reload = lxcReload,
};

int lxcRegister(void)
{
    virRegisterDriver(&lxcDriver);
    virRegisterStateDriver(&lxcStateDriver);
    return 0;
}

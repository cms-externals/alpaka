/* Copyright 2022 Benjamin Worpitz, Jan Stephan
 *
 * This file is part of Alpaka.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#ifdef ALPAKA_ACC_ANY_BT_OACC_ENABLED

#    if _OPENACC < 201306
#        error If ALPAKA_ACC_ANY_BT_OACC_ENABLED is set, the compiler has to support OpenACC 2.0 or higher!
#    endif

#    include <alpaka/dev/Traits.hpp>
#    include <alpaka/mem/buf/Traits.hpp>
#    include <alpaka/pltf/PltfOacc.hpp>
#    include <alpaka/pltf/Traits.hpp>
#    include <alpaka/queue/Properties.hpp>
#    include <alpaka/queue/QueueGenericThreadsBlocking.hpp>
#    include <alpaka/queue/QueueGenericThreadsNonBlocking.hpp>
#    include <alpaka/queue/Traits.hpp>
#    include <alpaka/queue/cpu/IGenericThreadsQueue.hpp>
#    include <alpaka/wait/Traits.hpp>

#    include <openacc.h>

#    include <memory>
#    include <mutex>
#    include <sstream>
#    include <vector>

namespace alpaka
{
    class DevOacc;

    namespace oacc
    {
        namespace detail
        {
            //! The OpenACC device implementation.
            class DevOaccImpl
            {
            public:
                DevOaccImpl(int iDevice) noexcept : m_deviceType(::acc_get_device_type()), m_iDevice(iDevice)
                {
                    makeCurrent();
                    m_gridsLock = reinterpret_cast<std::uint32_t*>(acc_malloc(2 * sizeof(std::uint32_t)));
                    const auto gridsLock = m_gridsLock;
#    pragma acc parallel loop vector default(present) deviceptr(gridsLock)
                    for(std::size_t a = 0; a < 2; ++a)
                        gridsLock[a] = 0u;
                }

                ALPAKA_FN_HOST auto getAllExistingQueues() const
                    -> std::vector<std::shared_ptr<IGenericThreadsQueue<DevOacc>>>
                {
                    std::vector<std::shared_ptr<IGenericThreadsQueue<DevOacc>>> vspQueues;

                    std::lock_guard<std::mutex> lk(m_Mutex);
                    vspQueues.reserve(std::size(m_queues));

                    for(auto it = std::begin(m_queues); it != std::end(m_queues);)
                    {
                        auto spQueue(it->lock());
                        if(spQueue)
                        {
                            vspQueues.emplace_back(std::move(spQueue));
                            ++it;
                        }
                        else
                        {
                            it = m_queues.erase(it);
                        }
                    }
                    return vspQueues;
                }

                //! Registers the given queue on this device.
                //! NOTE: Every queue has to be registered for correct functionality of device wait operations!
                ALPAKA_FN_HOST auto registerQueue(std::shared_ptr<IGenericThreadsQueue<DevOacc>> spQueue) -> void
                {
                    std::lock_guard<std::mutex> lk(m_Mutex);

                    // Register this queue on the device.
                    m_queues.push_back(std::move(spQueue));
                }

                int iDevice() const
                {
                    return m_iDevice;
                }
                acc_device_t deviceType() const
                {
                    return m_deviceType;
                }

                void makeCurrent() const
                {
#    if ALPAKA_DEBUG >= ALPAKA_DEBUG_FULL
                    std::cout << "acc_set_device_num( " << iDevice() << ", [type] )" << std::endl;
#    endif
                    acc_set_device_num(iDevice(), deviceType());
                }

                std::uint32_t* gridsLock() const
                {
                    return m_gridsLock;
                }

                //! Create and/or return staticlly mapped device pointer of host address.
                template<typename TElem, typename TExtent>
                ALPAKA_FN_HOST auto mapStatic(TElem* pHost, TExtent const& extent) -> TElem*
                {
                    makeCurrent();
                    void* pDev = acc_deviceptr(pHost);
                    if(!pDev)
                    {
#    pragma acc enter data create(pHost [0:extent.prod()])
                        pDev = acc_deviceptr(pHost);
                        assert(pDev != nullptr);
                    }
                    return reinterpret_cast<TElem*>(pDev);
                }

            private:
                std::mutex mutable m_Mutex;
                std::vector<std::weak_ptr<IGenericThreadsQueue<DevOacc>>> mutable m_queues;
                acc_device_t m_deviceType;
                int m_iDevice;
                std::uint32_t* m_gridsLock = nullptr;
            };
        } // namespace detail
    } // namespace oacc

    //! The OpenACC device handle.
    class DevOacc
        : public concepts::Implements<ConceptCurrentThreadWaitFor, DevOacc>
        , public concepts::Implements<ConceptDev, DevOacc>
    {
        friend struct traits::GetDevByIdx<PltfOacc>;

        static auto& device(int iDevice)
        {
            static std::vector<std::unique_ptr<oacc::detail::DevOaccImpl>> devices(getDevCount<PltfOacc>());
            static std::mutex mutex;

            auto& d = devices.at(static_cast<unsigned>(iDevice));
            if(!d)
            {
                std::lock_guard<std::mutex> lock(mutex);
                if(!d)
                    d = std::make_unique<oacc::detail::DevOaccImpl>(iDevice);
            }
            return *d;
        }

    protected:
        DevOacc(int iDevice) : m_devOaccImpl(device(iDevice))
        {
        }

    public:
        ALPAKA_FN_HOST auto operator==(DevOacc const& rhs) const -> bool
        {
            return m_devOaccImpl.iDevice() == rhs.m_devOaccImpl.iDevice();
        }
        ALPAKA_FN_HOST auto operator!=(DevOacc const& rhs) const -> bool
        {
            return !((*this) == rhs);
        }
        int iDevice() const
        {
            return m_devOaccImpl.iDevice();
        }
        acc_device_t deviceType() const
        {
            return m_devOaccImpl.deviceType();
        }
        void makeCurrent() const
        {
            m_devOaccImpl.makeCurrent();
        }

        std::uint32_t* gridsLock() const
        {
            return m_devOaccImpl.gridsLock();
        }

        ALPAKA_FN_HOST auto getAllQueues() const -> std::vector<std::shared_ptr<IGenericThreadsQueue<DevOacc>>>
        {
            return m_devOaccImpl.getAllExistingQueues();
        }

        //! Create and/or return staticlly mapped device pointer of host address.
        template<typename TElem, typename TExtent>
        ALPAKA_FN_HOST auto mapStatic(TElem* pHost, TExtent const& extent) const -> TElem*
        {
            return m_devOaccImpl.mapStatic(pHost, extent);
        }

        //! Registers the given queue on this device.
        //! NOTE: Every queue has to be registered for correct functionality of device wait operations!
        ALPAKA_FN_HOST auto registerQueue(std::shared_ptr<IGenericThreadsQueue<DevOacc>> spQueue) const -> void
        {
            m_devOaccImpl.registerQueue(spQueue);
        }

    private:
        oacc::detail::DevOaccImpl& m_devOaccImpl;
    };

    namespace traits
    {
        //! The OpenACC device name get trait specialization.
        template<>
        struct GetName<DevOacc>
        {
            ALPAKA_FN_HOST static auto getName(DevOacc const&) -> std::string
            {
                return std::string("OpenACC target");
            }
        };

        //! The OpenACC device available memory get trait specialization.
        template<>
        struct GetMemBytes<DevOacc>
        {
            ALPAKA_FN_HOST static auto getMemBytes(DevOacc const& dev) -> std::size_t
            {
                return acc_get_property(dev.iDevice(), dev.deviceType(), acc_property_memory);
            }
        };

        //! The OpenACC device free memory get trait specialization.
        template<>
        struct GetFreeMemBytes<DevOacc>
        {
            ALPAKA_FN_HOST static auto getFreeMemBytes(DevOacc const& dev) -> std::size_t
            {
                return acc_get_property(dev.iDevice(), dev.deviceType(), acc_property_free_memory);
            }
        };

        //! The OpenACC device warp size get trait specialization.
        template<>
        struct GetWarpSize<DevOacc>
        {
            ALPAKA_FN_HOST static auto getWarpSize(DevOacc const& /* dev */) -> std::size_t
            {
                return 1u;
            }
        };

        //! The OpenACC device reset trait specialization.
        template<>
        struct Reset<DevOacc>
        {
            ALPAKA_FN_HOST static auto reset(DevOacc const& /* dev */) -> void
            {
                //! \TODO
            }
        };
    } // namespace traits

    template<typename TElem, typename TDim, typename TIdx>
    class BufOacc;

    namespace traits
    {
        //! The OpenACC device memory buffer type trait specialization.
        template<typename TElem, typename TDim, typename TIdx>
        struct BufType<DevOacc, TElem, TDim, TIdx>
        {
            using type = BufOacc<TElem, TDim, TIdx>;
        };

        //! The OpenACC device platform type trait specialization.
        template<>
        struct PltfType<DevOacc>
        {
            using type = PltfOacc;
        };
    } // namespace traits

    using QueueOaccNonBlocking = QueueGenericThreadsNonBlocking<DevOacc>;
    using QueueOaccBlocking = QueueGenericThreadsBlocking<DevOacc>;

    namespace traits
    {
        template<>
        struct QueueType<DevOacc, Blocking>
        {
            using type = QueueOaccBlocking;
        };

        template<>
        struct QueueType<DevOacc, NonBlocking>
        {
            using type = QueueOaccNonBlocking;
        };

        //! The thread OpenACC device wait specialization.
        //!
        //! Blocks until the device has completed all preceding requested tasks.
        //! Tasks that are enqueued or queues that are created after this call is made are not waited for.
        template<>
        struct CurrentThreadWaitFor<DevOacc>
        {
            ALPAKA_FN_HOST static auto currentThreadWaitFor(DevOacc const& dev) -> void
            {
                ALPAKA_DEBUG_FULL_LOG_SCOPE;

                generic::currentThreadWaitForDevice(dev);
            }
        };

        //! The OpenACC platform device type trait specialization.
        template<>
        struct DevType<PltfOacc>
        {
            using type = DevOacc;
        };

        //! The OpenACC platform device get trait specialization.
        template<>
        struct GetDevByIdx<PltfOacc>
        {
            //! \param devIdx device id, less than GetDevCount
            ALPAKA_FN_HOST static auto getDevByIdx(std::size_t devIdx) -> DevOacc
            {
                ALPAKA_DEBUG_FULL_LOG_SCOPE;

                std::size_t const devCount(getDevCount<PltfOacc>());
                if(devIdx >= devCount)
                {
                    std::stringstream ssErr;
                    ssErr << "Unable to return device handle for OpenACC device with index " << devIdx
                          << " because there are only " << devCount << " devices!";
                    throw std::runtime_error(ssErr.str());
                }

                return {static_cast<int>(devIdx)};
            }
        };

    } // namespace traits
} // namespace alpaka

#endif

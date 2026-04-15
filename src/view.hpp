#pragma once

#include <cstddef>
#include <tuple>
#include <utility>

#include "view_traits.hpp"
#include "view_plan.hpp"

namespace ecs {

template <typename RegistryType, typename... Components>
class BasicView {
    static_assert(sizeof...(Components) > 0, "views require at least one component type");
    static_assert(detail::unique_component_types<Components...>::value,
                  "views cannot contain duplicate component types");

public:
    explicit BasicView(RegistryType& registry)
        : registry_(&registry) {}

    template <typename Func>
    void forEach(Func&& func) const {
        auto&& callback = func;
        const auto plan = detail::make_view_plan<RegistryType, Components...>(*registry_);
        if (plan.anchor_storage == nullptr) {
            return;
        }

        forEachPlanned(callback, plan, std::make_index_sequence<sizeof...(Components)>{});
    }

private:
    template <typename Component>
    detail::component_pointer_t<RegistryType, Component> fetch(Entity entity) const {
        using Base = detail::component_base_t<Component>;
        return registry_->template try_get<Base>(entity);
    }

    template <typename Tuple, std::size_t... Indices>
    static bool all_present(const Tuple& components, std::index_sequence<Indices...>) {
        return ((std::get<Indices>(components) != nullptr) && ...);
    }

    template <typename Func, typename Tuple, std::size_t... Indices>
    static void invoke(Func& func, Entity entity, Tuple& components, std::index_sequence<Indices...>) {
        func(entity, *std::get<Indices>(components)...);
    }

    template <typename Func, std::size_t... Indices>
    void forEachPlanned(Func& func,
                        const detail::ViewPlan<RegistryType, Components...>& plan,
                        std::index_sequence<Indices...>) const {
        const bool handled = ((plan.anchor_index == Indices && (iterate<Indices>(func, plan.anchor_storage), true)) || ...);
        (void)handled;
    }

    template <std::size_t Index, typename Func>
    void iterate(Func& func, const RawPagedSparseArray* anchor_storage) const {
        const auto& dense_entities = anchor_storage->entities();
        const std::size_t count = anchor_storage->size();
        for (std::size_t i = 0; i < count; ++i) {
            const Entity entity = dense_entities[i];
            auto components = std::tuple<detail::component_pointer_t<RegistryType, Components>...>{
                fetch<Components>(entity)...};

            if (all_present(components, std::index_sequence_for<Components...>{})) {
                invoke(func, entity, components, std::index_sequence_for<Components...>{});
            }
        }
    }

    RegistryType* registry_;
};

}  // namespace ecs

"""High-level consumer setup after an app obtains a stream descriptor."""

from ndnsf import (
    LiveStreamItemAdmission,
    ServiceUser,
    StreamSubscriptionOptions,
)


def subscribe(user: ServiceUser, descriptor):
    return user.subscribe_stream(
        descriptor,
        StreamSubscriptionOptions(
            on_item=lambda item: (
                print(item.cursor, item.original_name, len(item.content)),
                LiveStreamItemAdmission.accept_item(),
            )[1],
        ),
    )


# ServiceUser starts the predictive subscriber before returning it.
# subscriber = subscribe(user, predictive_descriptor)
